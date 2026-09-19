#ifndef CLUSTER_ROUTE_TRANSPORT_HPP
#define CLUSTER_ROUTE_TRANSPORT_HPP

#include "redis.hpp"
#include "json.hpp"

#include <atomic>
#include <functional>
#include <thread>
#include <optional>
#include <string>

namespace cluster {

struct RoutedMessage {
    std::string routeId;
    std::string sourceNode;
    std::string targetNode;
    int messageType = 0;
    int senderUserId = 0;
    int receiverUserId = 0;
    int hops = 0;
    nlohmann::json payload;

    std::string encode() const;
    static std::optional<RoutedMessage> decode(const std::string& encoded);
};

// Infrastructure failures remain in the PEL; business-level retries still
// obey the Phase 5 dead-letter bound.
enum class RouteOutcome { Accepted, Retry, RetryInfrastructure, Terminal };
struct RouteMetrics {
    std::uint64_t produced = 0, consumed = 0, acked = 0;
    std::uint64_t retried = 0, claimed = 0, duplicate = 0, failed = 0;
    std::uint64_t pending = 0;
    std::uint64_t streamLength = 0, streamPeak = 0;
    std::uint64_t oldestPendingMs = 0, lag = 0, overloadRejected = 0;
    std::uint64_t targetStreamPeak = 0;
};

// 每节点一条 Stream + Consumer Group；routeId 与目标节点 deliveryId 不同。
class RouteTransport final {
public:
    RouteTransport(Redis& redis, std::string nodeId, std::string generation = "");
    ~RouteTransport();
    bool start(std::function<RouteOutcome(RoutedMessage)> handler);
    void stop();
    bool send(const RoutedMessage& message);
    // Accepted / Duplicate / Error. Marker is written only after local Pending accepted.
    enum class DedupResult { New, Duplicate, Error };
    DedupResult markAccepted(const std::string& routeId);
    // Production snapshots are cached; explicit fresh=true is for diagnostics/tests.
    RouteMetrics metrics(bool fresh = false) const;
    std::string nextRouteId();
    static std::string streamFor(const std::string& nodeId);

private:
    void consume();
    bool ack(const std::string& entryId);
    bool handleEntry(const std::string& entryId, const std::string& payload,
                     int attempts);
    Redis& _redis;
    std::string _nodeId;
    std::string _generation;
    std::string _group;
    std::string _consumer;
    std::function<RouteOutcome(RoutedMessage)> _handler;
    std::thread _worker;
    std::atomic<bool> _stopping{false};
    std::atomic<std::uint64_t> _next{1};
    std::atomic<std::uint64_t> _produced{0}, _consumed{0}, _acked{0},
        _retried{0}, _claimed{0}, _duplicate{0}, _failed{0};
    std::atomic<std::uint64_t> _streamLength{0}, _streamPeak{0},
        _pending{0}, _oldestPendingMs{0}, _lag{0}, _overloadRejected{0},
        _targetStreamPeak{0};
};

} // namespace cluster
#endif
