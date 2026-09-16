#ifndef GATEWAY_MESSAGE_ENVELOPE_HPP
#define GATEWAY_MESSAGE_ENVELOPE_HPP

#include "json.hpp"
#include "protocol/protocol_state.hpp"

#include <utility>

namespace gateway
{

// 解码后的内部请求。它描述业务消息，不复用 Protobuf wire Envelope。
// Phase 2 不分配 message_id；消息身份与投递状态留到 Phase 3。
class MessageEnvelope final
{
public:
    MessageEnvelope(int messageType,
                    nlohmann::json payload,
                    protocol::WireProtocol sourceProtocol)
        : _messageType(messageType),
          _payload(std::move(payload)),
          _sourceProtocol(sourceProtocol)
    {
    }

    int messageType() const noexcept { return _messageType; }
    const nlohmann::json& payload() const noexcept { return _payload; }
    protocol::WireProtocol sourceProtocol() const noexcept
    {
        return _sourceProtocol;
    }

private:
    int _messageType;
    nlohmann::json _payload;
    protocol::WireProtocol _sourceProtocol;
};

} // namespace gateway

#endif
