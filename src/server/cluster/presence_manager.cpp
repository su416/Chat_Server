#include "cluster/presence_manager.hpp"

#include <muduo/base/Logging.h>
#include <algorithm>
#include <chrono>
#include <cctype>
#include <stdexcept>
#include <unistd.h>

namespace cluster {
namespace {
constexpr const char* kCompareDelete =
    "if redis.call('GET',KEYS[1]) == ARGV[1] then "
    "return redis.call('DEL',KEYS[1]) else return 0 end";
constexpr const char* kCompareExpire =
    "if redis.call('GET',KEYS[1]) == ARGV[1] then "
    "return redis.call('EXPIRE',KEYS[1],ARGV[2]) else return 0 end";
constexpr const char* kBind =
    "if redis.call('GET',KEYS[2]) ~= ARGV[2] then return 0 end "
    "redis.call('SET',KEYS[1],ARGV[1],'EX',ARGV[3]); return 1";
bool isOne(const RedisReplyPtr& reply)
{
    return reply && reply->type == REDIS_REPLY_INTEGER && reply->integer == 1;
}
}

PresenceManager::PresenceManager(Redis& redis, std::string nodeId, int leaseSeconds)
    : _redis(redis), _nodeId(std::move(nodeId)), _leaseSeconds(leaseSeconds)
{
    if (_nodeId.empty() || _leaseSeconds < 3 ||
        !std::all_of(_nodeId.begin(), _nodeId.end(), [](unsigned char c) {
            return std::isalnum(c) || c == '_' || c == '-';
        }))
        throw std::invalid_argument("invalid node_id or lease");
    const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    _generation = std::to_string(micros) + "-" + std::to_string(getpid());
}

std::string PresenceManager::userKey(int userId)
{
    return "chat:presence:user:" + std::to_string(userId);
}

std::string PresenceManager::nodeKey(const std::string& nodeId)
{
    return "chat:node:" + nodeId;
}

std::string PresenceManager::valueFor(std::uint64_t sessionId) const
{
    return _nodeId + "|" + _generation + "|" + std::to_string(sessionId);
}

bool PresenceManager::registerNode()
{
    // 同名节点启动时新实例赢；旧实例的 heartbeat 会失败并停止续租。
    auto reply = _redis.command({"SET", nodeKey(_nodeId), _generation,
                                 "EX", std::to_string(_leaseSeconds)});
    return reply && reply->type == REDIS_REPLY_STATUS &&
           std::string(reply->str, reply->len) == "OK";
}

bool PresenceManager::heartbeat(
    const std::vector<std::pair<int, std::uint64_t>>& localSessions)
{
    auto node = _redis.command({"EVAL", kCompareExpire, "1", nodeKey(_nodeId),
                                _generation, std::to_string(_leaseSeconds)});
    if (!isOne(node))
    {
        auto current = _redis.command({"GET", nodeKey(_nodeId)});
        if (!current || current->type != REDIS_REPLY_NIL)
        {
            LOG_ERROR << "NODE_HEARTBEAT lost ownership node=" << _nodeId;
            return false;
        }
        auto recreated = _redis.command({"SET", nodeKey(_nodeId), _generation,
                                         "NX", "EX", std::to_string(_leaseSeconds)});
        if (!recreated || recreated->type != REDIS_REPLY_STATUS)
            return false;
    }
    ++_heartbeat;
    // O(local users) Redis work, but only O(1) timers and 256-command pipeline
    // batches; 10k users do not cause 10k network round trips.
    constexpr std::size_t kBatchSize = 256;
    for (std::size_t offset = 0; offset < localSessions.size(); offset += kBatchSize)
    {
        const auto end = std::min(offset + kBatchSize, localSessions.size());
        std::vector<std::vector<std::string>> commands;
        commands.reserve(end - offset);
        for (std::size_t index = offset; index < end; ++index)
        {
            const auto& [userId, sessionId] = localSessions[index];
            commands.push_back({"EVAL", kCompareExpire, "1", userKey(userId),
                                valueFor(sessionId), std::to_string(_leaseSeconds)});
        }
        auto replies = _redis.batch(commands);
        if (replies.size() != commands.size())
        {
            LOG_ERROR << "PRESENCE_REFRESH batch failed node=" << _nodeId;
            return false;
        }
        for (std::size_t index = 0; index < replies.size(); ++index)
            if (!isOne(replies[index]))
                LOG_WARN << "PRESENCE_REFRESH stale user=" << localSessions[offset + index].first
                         << " node=" << _nodeId;
    }
    return true;
}

bool PresenceManager::unregisterNode()
{
    return isOne(_redis.command({"EVAL", kCompareDelete, "1", nodeKey(_nodeId),
                                  _generation}));
}

bool PresenceManager::bindUser(int userId, std::uint64_t sessionId)
{
    if (userId <= 0 || sessionId == 0) return false;
    auto result = _redis.command({"EVAL", kBind, "2", userKey(userId), nodeKey(_nodeId),
                                  valueFor(sessionId), _generation,
                                  std::to_string(_leaseSeconds)});
    if (isOne(result)) { ++_bind; return true; }
    return false;
}

bool PresenceManager::unbindUser(int userId, std::uint64_t sessionId)
{
    if (userId <= 0 || sessionId == 0) return false;
    auto result = _redis.command({"EVAL", kCompareDelete, "1", userKey(userId),
                                  valueFor(sessionId)});
    if (isOne(result)) { ++_unbind; return true; }
    return false;
}

LookupResult PresenceManager::lookupUser(int userId)
{
    ++_lookup;
    if (userId <= 0) return {LookupStatus::Miss, {}};
    auto result = _redis.command({"GET", userKey(userId)});
    if (!result || result->type == REDIS_REPLY_ERROR) return {LookupStatus::Error, {}};
    if (result->type == REDIS_REPLY_NIL) { ++_miss; return {LookupStatus::Miss, {}}; }
    if (result->type != REDIS_REPLY_STRING) return {LookupStatus::Error, {}};
    std::string value(result->str, result->len);
    auto first = value.find('|');
    auto second = first == std::string::npos ? first : value.find('|', first + 1);
    if (first == std::string::npos || second == std::string::npos)
        return {LookupStatus::Error, {}};
    PresenceOwner owner{value.substr(0, first), value.substr(first + 1, second - first - 1), 0};
    try { owner.sessionId = std::stoull(value.substr(second + 1)); }
    catch (...) { return {LookupStatus::Error, {}}; }
    auto node = _redis.command({"GET", nodeKey(owner.nodeId)});
    if (!node || node->type == REDIS_REPLY_ERROR) return {LookupStatus::Error, {}};
    if (node->type != REDIS_REPLY_STRING ||
        std::string(node->str, node->len) != owner.generation)
    {
        // Dead/replaced node: compare-and-delete avoids deleting a concurrent rebind.
        _redis.command({"EVAL", kCompareDelete, "1", userKey(userId), value});
        ++_miss;
        return {LookupStatus::Miss, {}};
    }
    ++_hit;
    return {LookupStatus::Hit, owner};
}

bool PresenceManager::ownsUser(int userId, std::uint64_t sessionId)
{
    auto result = lookupUser(userId);
    return result.status == LookupStatus::Hit && result.owner.nodeId == _nodeId &&
           result.owner.generation == _generation && result.owner.sessionId == sessionId;
}

PresenceMetrics PresenceManager::metrics() const
{
    return {_bind.load(), _unbind.load(), _lookup.load(), _hit.load(), _miss.load(),
            _heartbeat.load()};
}
} // namespace cluster
