#include "gateway/message_router.hpp"

#include <muduo/base/Logging.h>

#include <exception>
#include <utility>

namespace gateway
{

MessageRouter::MessageRouter(HandlerResolver resolver)
    : _resolveHandler(std::move(resolver))
{
}

DispatchResult MessageRouter::dispatch(
    const muduo::net::TcpConnectionPtr& connection,
    const MessageEnvelope& message,
    muduo::Timestamp receivedAt) const noexcept
{
    try
    {
        auto handler = _resolveHandler(message.messageType());
        if (!handler)
        {
            return DispatchResult::UnknownMessageType;
        }

        // 仅此处把规范化的内部消息适配给尚未迁移的 JSON 业务 handler。
        auto request = message.payload();
        (*handler)(connection, request, receivedAt);
        return DispatchResult::Handled;
    }
    catch (const std::exception& error)
    {
        LOG_WARN << "message handler rejected request: " << error.what();
        return DispatchResult::HandlerError;
    }
    catch (...)
    {
        LOG_WARN << "message handler rejected request: unknown exception";
        return DispatchResult::HandlerError;
    }
}

} // namespace gateway
