#ifndef GATEWAY_MESSAGE_ENVELOPE_HPP
#define GATEWAY_MESSAGE_ENVELOPE_HPP

#include "json.hpp"
#include "protocol/protocol_state.hpp"

#include <cstdint>
#include <optional>
#include <string>
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
                    protocol::WireProtocol sourceProtocol,
                    std::optional<std::uint64_t> deliveryId = std::nullopt)
        : _messageType(messageType),
          _payload(std::move(payload)),
          _sourceProtocol(sourceProtocol),
          _deliveryId(deliveryId)
    {
        if (_deliveryId)
        {
            _payload["delivery_id"] = std::to_string(*_deliveryId);
        }
    }

    int messageType() const noexcept { return _messageType; }
    const nlohmann::json& payload() const noexcept { return _payload; }
    protocol::WireProtocol sourceProtocol() const noexcept
    {
        return _sourceProtocol;
    }
    std::optional<std::uint64_t> deliveryId() const noexcept
    {
        return _deliveryId;
    }

private:
    int _messageType;
    nlohmann::json _payload;
    protocol::WireProtocol _sourceProtocol;
    std::optional<std::uint64_t> _deliveryId;
};

} // namespace gateway

#endif
