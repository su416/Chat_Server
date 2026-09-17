#ifndef GATEWAY_PENDING_DELIVERY_MANAGER_HPP
#define GATEWAY_PENDING_DELIVERY_MANAGER_HPP

#include "gateway/message_envelope.hpp"
#include "gateway/session.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace gateway
{

struct DeliveryPolicy
{
    std::chrono::milliseconds ackTimeout{2000};
    std::uint32_t maxRetries = 2;
    std::size_t maxPending = 10000;
    std::size_t maxPendingBytes = 16 * 1024 * 1024;
};

struct DeliveryMetrics
{
    std::uint64_t sent = 0;
    std::uint64_t acked = 0;
    std::uint64_t retried = 0;
    std::uint64_t timedOut = 0;
    std::uint64_t failed = 0;
    std::size_t pending = 0;
    std::size_t pendingPeak = 0;
    std::size_t pendingBytes = 0;
    std::size_t pendingPeakBytes = 0;
};

enum class AckResult { Acked, Unknown, WrongSession };

struct RetryAction
{
    UserId receiverId;
    std::weak_ptr<const Session> session;
    MessageEnvelope envelope;
    std::uint32_t retryCount;
};

// 仅承载本节点、当前 Session 的在线单聊投递。锁内不做网络/数据库操作。
class PendingDeliveryManager final
{
public:
    using Clock = std::chrono::steady_clock;

    explicit PendingDeliveryManager(DeliveryPolicy policy = {});

    std::uint64_t nextDeliveryId();
    bool track(const std::shared_ptr<const Session>& session,
               const MessageEnvelope& envelope,
               Clock::time_point now = Clock::now());
    AckResult ack(std::uint64_t deliveryId,
                  const muduo::net::TcpConnectionPtr& connection);
    std::vector<RetryAction> scanExpired(Clock::time_point now = Clock::now());
    std::size_t cancelConnection(
        const muduo::net::TcpConnectionPtr& connection);
    DeliveryMetrics metrics() const;

private:
    struct PendingMessage
    {
        UserId receiverId;
        MessageEnvelope envelope;
        std::weak_ptr<const Session> session;
        Clock::time_point lastSend;
        std::uint32_t retryCount = 0;
        std::size_t payloadBytes = 0;
    };

    const DeliveryPolicy _policy;
    std::atomic<std::uint64_t> _nextId;
    mutable std::mutex _mutex;
    std::unordered_map<std::uint64_t, PendingMessage> _pending;
    std::size_t _pendingBytes = 0;
    DeliveryMetrics _metrics;
};

} // namespace gateway

#endif
