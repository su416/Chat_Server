#include "cluster/route_transport.hpp"

#include <muduo/base/Logging.h>
#include <chrono>

namespace cluster {

std::string RoutedMessage::encode() const
{
    return nlohmann::json{{"version", 1}, {"route_id", routeId},
        {"source_node", sourceNode}, {"target_node", targetNode},
        {"message_type", messageType}, {"sender_user_id", senderUserId},
        {"receiver_user_id", receiverUserId}, {"hops", hops},
        {"payload", payload}}.dump();
}

std::optional<RoutedMessage> RoutedMessage::decode(const std::string& encoded)
{
    try
    {
        auto parsed = nlohmann::json::parse(encoded);
        if (!parsed.is_object() || parsed.at("version") != 1 ||
            !parsed.at("payload").is_object()) return std::nullopt;
        const auto& clientPayload = parsed.at("payload");
        if (!clientPayload.contains("msgid") ||
            !clientPayload.at("msgid").is_number_integer()) return std::nullopt;
        RoutedMessage message;
        message.routeId = parsed.at("route_id").get<std::string>();
        message.sourceNode = parsed.at("source_node").get<std::string>();
        message.targetNode = parsed.at("target_node").get<std::string>();
        message.messageType = parsed.at("message_type").get<int>();
        message.senderUserId = parsed.at("sender_user_id").get<int>();
        message.receiverUserId = parsed.at("receiver_user_id").get<int>();
        message.hops = parsed.at("hops").get<int>();
        message.payload = parsed.at("payload");
        if (message.routeId.empty() || message.sourceNode.empty() ||
            message.targetNode.empty() || message.receiverUserId <= 0 ||
            message.hops < 0 || message.hops > 1 || message.messageType <= 0 ||
            clientPayload.at("msgid").get<int>() != message.messageType)
            return std::nullopt;
        return message;
    }
    catch (...) { return std::nullopt; }
}

RouteTransport::RouteTransport(Redis& redis, std::string nodeId)
    : _redis(redis), _nodeId(std::move(nodeId)) {}

std::string RouteTransport::channelFor(const std::string& nodeId)
{
    return "chat:route:node:" + nodeId;
}

bool RouteTransport::start(std::function<void(RoutedMessage)> handler)
{
    return _redis.subscribeNode(channelFor(_nodeId), [handler = std::move(handler)](
                                     const std::string& payload) {
        auto message = RoutedMessage::decode(payload);
        if (!message) { LOG_WARN << "ROUTE_FAILED invalid internal message"; return; }
        handler(std::move(*message));
    });
}

bool RouteTransport::send(const RoutedMessage& message)
{
    if (message.targetNode.empty() || message.hops > 1) return false;
    return _redis.publish(channelFor(message.targetNode), message.encode());
}

std::string RouteTransport::nextRouteId()
{
    const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    return _nodeId + ":" + std::to_string(millis) + ":" +
           std::to_string(_next.fetch_add(1));
}
} // namespace cluster
