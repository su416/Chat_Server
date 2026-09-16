#include "protocol/message_encoder.hpp"

#include "chat.pb.h"
#include "jsoncodec.hpp"
#include "protocodec.hpp"
#include "protocol/frame_decode_result.hpp"

#include <stdexcept>

namespace protocol
{

std::string MessageEncoder::encode(const gateway::MessageEnvelope& message,
                                   WireProtocol protocol)
{
    const std::string jsonBody = message.payload().dump();
    if (protocol == WireProtocol::Protobuf)
    {
        chat::Envelope wireEnvelope;
        wireEnvelope.set_msgid(message.messageType());
        if (message.payload().contains("errno") &&
            message.payload()["errno"].is_number_integer())
        {
            wireEnvelope.set_errcode(message.payload()["errno"].get<int>());
        }
        wireEnvelope.set_payload(jsonBody);
        const std::string serialized = wireEnvelope.SerializeAsString();
        if (serialized.size() > kDefaultMaxFrameSize)
        {
            throw std::length_error("outbound Protobuf frame exceeds limit");
        }
        return ProtoCodec::encode(serialized);
    }

    if (jsonBody.size() > kDefaultMaxFrameSize)
    {
        throw std::length_error("outbound JSON frame exceeds limit");
    }
    return JsonCodec::encode(jsonBody);
}

} // namespace protocol
