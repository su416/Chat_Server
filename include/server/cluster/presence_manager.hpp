#ifndef CLUSTER_PRESENCE_MANAGER_HPP
#define CLUSTER_PRESENCE_MANAGER_HPP

#include "redis.hpp"

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace cluster {

struct PresenceOwner {
    std::string nodeId;
    std::string generation;
    std::uint64_t sessionId = 0;
};

enum class LookupStatus { Hit, Miss, Error };
struct LookupResult {
    LookupStatus status = LookupStatus::Error;
    PresenceOwner owner;
};

struct PresenceMetrics {
    std::uint64_t bind = 0, unbind = 0, lookup = 0, hit = 0, miss = 0;
    std::uint64_t nodeHeartbeat = 0;
};

// Redis 仅保存租约和归属，不持有 TcpConnection。单个节点共用一个 heartbeat timer。
class PresenceManager final {
public:
    PresenceManager(Redis& redis, std::string nodeId,
                    int leaseSeconds = 20);
    const std::string& nodeId() const { return _nodeId; }
    const std::string& generation() const { return _generation; }
    bool registerNode();
    bool heartbeat(const std::vector<std::pair<int, std::uint64_t>>& localSessions);
    bool unregisterNode();
    bool bindUser(int userId, std::uint64_t sessionId);
    bool unbindUser(int userId, std::uint64_t sessionId);
    LookupResult lookupUser(int userId);
    bool ownsUser(int userId, std::uint64_t sessionId);
    PresenceMetrics metrics() const;

private:
    std::string valueFor(std::uint64_t sessionId) const;
    static std::string userKey(int userId);
    static std::string nodeKey(const std::string& nodeId);

    Redis& _redis;
    std::string _nodeId;
    std::string _generation;
    int _leaseSeconds;
    std::atomic<std::uint64_t> _bind{0}, _unbind{0}, _lookup{0}, _hit{0}, _miss{0}, _heartbeat{0};
};

} // namespace cluster
#endif
