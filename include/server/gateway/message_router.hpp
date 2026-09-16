#ifndef GATEWAY_MESSAGE_ROUTER_HPP
#define GATEWAY_MESSAGE_ROUTER_HPP

#include "chatservice.hpp"
#include "gateway/message_envelope.hpp"

#include <functional>
#include <optional>

namespace gateway
{

enum class DispatchResult { Handled, UnknownMessageType, HandlerError };

// 统一消息进入现有 ChatService handler 的单一适配边界。
class MessageRouter final
{
public:
    using HandlerResolver = std::function<std::optional<MsgHandler>(int)>;

    explicit MessageRouter(HandlerResolver resolver);

    DispatchResult dispatch(const muduo::net::TcpConnectionPtr& connection,
                            const MessageEnvelope& message,
                            muduo::Timestamp receivedAt) const noexcept;

private:
    HandlerResolver _resolveHandler;
};

} // namespace gateway

#endif
