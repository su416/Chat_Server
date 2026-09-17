#include "gateway/pending_delivery_manager.hpp"

#include <muduo/base/Logging.h>

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace gateway
{
namespace
{
std::uint64_t initialDeliveryId()
{
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    // 单进程内随后只用原子递增；时间前缀不依赖数据库或 Reactor I/O。
    return static_cast<std::uint64_t>(milliseconds) << 20;
}
} // namespace

PendingDeliveryManager::PendingDeliveryManager(DeliveryPolicy policy)
    : _policy(policy), _nextId(initialDeliveryId())
{
    if (_policy.ackTimeout.count() <= 0 || _policy.maxPending == 0 ||
        _policy.maxPendingBytes == 0)
    {
        throw std::invalid_argument("invalid delivery policy");
    }
}

std::uint64_t PendingDeliveryManager::nextDeliveryId()
{
    auto current = _nextId.load(std::memory_order_relaxed);
    for (;;)
    {
        if (current == 0 || current == std::numeric_limits<std::uint64_t>::max())
        {
            throw std::overflow_error("delivery id space exhausted");
        }
        if (_nextId.compare_exchange_weak(current, current + 1,
                                          std::memory_order_relaxed))
        {
            return current;
        }
    }
}

bool PendingDeliveryManager::track(const std::shared_ptr<const Session>& session,
                                   const MessageEnvelope& envelope,
                                   Clock::time_point now)
{
    if (!session || !session->connection() || !envelope.deliveryId() ||
        *envelope.deliveryId() == 0)
    {
        return false;
    }

    const auto payloadBytes = envelope.payload().dump().size();

    std::lock_guard<std::mutex> lock(_mutex);
    if (_pending.size() >= _policy.maxPending ||
        payloadBytes > _policy.maxPendingBytes - _pendingBytes)
    {
        return false;
    }
    const auto id = *envelope.deliveryId();
    const auto inserted = _pending.emplace(
        id, PendingMessage{session->userId(), envelope, session, now, 0,
                           payloadBytes}).second;
    if (inserted)
    {
        ++_metrics.sent;
        _pendingBytes += payloadBytes;
        _metrics.pendingPeak = std::max(_metrics.pendingPeak, _pending.size());
        _metrics.pendingPeakBytes = std::max(_metrics.pendingPeakBytes,
                                             _pendingBytes);
    }
    return inserted;
}

AckResult PendingDeliveryManager::ack(
    std::uint64_t deliveryId,
    const muduo::net::TcpConnectionPtr& connection)
{
    std::lock_guard<std::mutex> lock(_mutex);
    auto it = _pending.find(deliveryId);
    if (it == _pending.end())
    {
        return AckResult::Unknown;
    }
    auto session = it->second.session.lock();
    if (!connection || !session || session->connection() != connection)
    {
        return AckResult::WrongSession;
    }
    _pendingBytes -= it->second.payloadBytes;
    _pending.erase(it);
    ++_metrics.acked;
    return AckResult::Acked;
}

bool PendingDeliveryManager::cancel(std::uint64_t deliveryId)
{
    std::lock_guard<std::mutex> lock(_mutex);
    auto it = _pending.find(deliveryId);
    if (it == _pending.end()) return false;
    _pendingBytes -= it->second.payloadBytes;
    _pending.erase(it);
    ++_metrics.failed;
    return true;
}

std::vector<RetryAction> PendingDeliveryManager::scanExpired(
    Clock::time_point now)
{
    std::vector<RetryAction> actions;
    std::vector<std::uint64_t> expiredSessionIds;
    std::vector<std::uint64_t> timedOutIds;
    std::vector<std::uint64_t> exhaustedIds;
    std::unique_lock<std::mutex> lock(_mutex);
    for (auto it = _pending.begin(); it != _pending.end();)
    {
        auto& pending = it->second;
        if (pending.session.expired())
        {
            expiredSessionIds.push_back(it->first);
            ++_metrics.failed;
            _pendingBytes -= pending.payloadBytes;
            it = _pending.erase(it);
            continue;
        }
        if (now - pending.lastSend < _policy.ackTimeout)
        {
            ++it;
            continue;
        }
        ++_metrics.timedOut;
        timedOutIds.push_back(it->first);
        if (pending.retryCount >= _policy.maxRetries)
        {
            exhaustedIds.push_back(it->first);
            ++_metrics.failed;
            _pendingBytes -= pending.payloadBytes;
            it = _pending.erase(it);
            continue;
        }
        ++pending.retryCount;
        ++_metrics.retried;
        pending.lastSend = now;
        actions.push_back(RetryAction{pending.receiverId, pending.session,
                                      pending.envelope, pending.retryCount});
        ++it;
    }
    lock.unlock();
    for (const auto id : timedOutIds)
        LOG_DEBUG << "DELIVERY_TIMEOUT id=" << id;
    for (const auto id : expiredSessionIds)
        LOG_WARN << "DELIVERY_FAILED expired session id=" << id;
    for (const auto id : exhaustedIds)
        LOG_WARN << "DELIVERY_FAILED max retries id=" << id;
    return actions;
}

std::size_t PendingDeliveryManager::cancelConnection(
    const muduo::net::TcpConnectionPtr& connection)
{
    if (!connection) return 0;
    std::size_t removed = 0;
    std::lock_guard<std::mutex> lock(_mutex);
    for (auto it = _pending.begin(); it != _pending.end();)
    {
        auto session = it->second.session.lock();
        if (session && session->connection() == connection)
        {
            _pendingBytes -= it->second.payloadBytes;
            it = _pending.erase(it);
            ++removed;
            ++_metrics.failed;
        }
        else
        {
            ++it;
        }
    }
    return removed;
}

DeliveryMetrics PendingDeliveryManager::metrics() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    auto result = _metrics;
    result.pending = _pending.size();
    result.pendingBytes = _pendingBytes;
    return result;
}

} // namespace gateway
