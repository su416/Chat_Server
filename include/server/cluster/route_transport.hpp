#ifndef CLUSTER_ROUTE_TRANSPORT_HPP
#define CLUSTER_ROUTE_TRANSPORT_HPP

#include "redis.hpp"
#include "json.hpp"

#include <atomic>
#include <functional>
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

// Pub/Sub 是非持久传输。routeId 与目标节点生成的 deliveryId 不同。
class RouteTransport final {
public:
    RouteTransport(Redis& redis, std::string nodeId);
    bool start(std::function<void(RoutedMessage)> handler);
    bool send(const RoutedMessage& message);
    std::string nextRouteId();
    static std::string channelFor(const std::string& nodeId);

private:
    Redis& _redis;
    std::string _nodeId;
    std::atomic<std::uint64_t> _next{1};
};

} // namespace cluster
#endif
