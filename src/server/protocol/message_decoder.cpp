#include "protocol/message_decoder.hpp"

#include "binary_codec.hpp"
#include "chat.pb.h"
#include "gateway/delivery_id.hpp"
#include "jsoncodec.hpp"
#include "protocodec.hpp"
#include "public.hpp"

#include <cctype>
#include <exception>

namespace protocol
{
namespace
{

DecodeResult frameFailure(const FrameDecodeResult& result)
{
    if (result.error == FrameDecodeError::OversizedFrame)
    {
        return DecodeResult::failure(DecodeError::OversizedFrame,
                                     "frame exceeds configured limit");
    }
    return DecodeResult::failure(DecodeError::MalformedFrame,
                                 "frame header is malformed");
}

DecodeResult normalizedJson(std::string frame,
                            WireProtocol sourceProtocol,
                            std::optional<int> forcedMessageType = std::nullopt)
{
    try
    {
        auto payload = nlohmann::json::parse(frame);
        if (!payload.is_object())
        {
            return DecodeResult::failure(
                DecodeError::MalformedFrame,
                "JSON request must be an object");
        }

        int messageType = 0;
        if (forcedMessageType)
        {
            messageType = *forcedMessageType;
            payload["msgid"] = messageType;
        }
        else
        {
            if (!payload.contains("msgid") ||
                !payload["msgid"].is_number_integer())
            {
                return DecodeResult::failure(
                    DecodeError::MalformedFrame,
                    "JSON request must contain an integer msgid");
            }
            messageType = payload["msgid"].get<int>();
        }
        std::optional<std::uint64_t> deliveryId;
        if (messageType == MESSAGE_ACK)
        {
            deliveryId = gateway::parseDeliveryId(payload);
            if (!deliveryId)
            {
                return DecodeResult::failure(DecodeError::MalformedFrame,
                                             "invalid ACK delivery_id");
            }
        }
        return DecodeResult::complete(gateway::MessageEnvelope(
            messageType, std::move(payload), sourceProtocol, deliveryId));
    }
    catch (const std::exception& error)
    {
        return DecodeResult::failure(DecodeError::MalformedFrame,
                                     std::string("invalid JSON payload: ") +
                                         error.what());
    }
}

bool tryJsonObject(const std::string& payload, nlohmann::json& normalized)
{
    try
    {
        normalized = nlohmann::json::parse(payload);
        return normalized.is_object();
    }
    catch (const std::exception&)
    {
        return false;
    }
}

bool normalizeTypedProtobuf(int messageType,
                            const std::string& payload,
                            nlohmann::json& normalized)
{
    switch (messageType)
    {
    case LOGIN_MSG:
    {
        chat::LoginRequest request;
        if (!request.ParseFromString(payload)) return false;
        normalized["id"] = request.id();
        normalized["password"] = request.password();
        return true;
    }
    case REG_MSG:
    {
        chat::RegisterRequest request;
        if (!request.ParseFromString(payload)) return false;
        normalized["name"] = request.name();
        normalized["password"] = request.password();
        return true;
    }
    case ONE_CHAT_MSG:
    {
        chat::OneChatRequest request;
        if (!request.ParseFromString(payload)) return false;
        normalized["id"] = request.id();
        normalized["toid"] = request.toid();
        normalized["name"] = request.name();
        normalized["msg"] = request.msg();
        normalized["time"] = request.time();
        return true;
    }
    case QUERY_ONE_CHAT_HISTORY_MSG:
    {
        chat::QueryOneChatHistoryRequest request;
        if (!request.ParseFromString(payload)) return false;
        normalized["id"] = request.id();
        normalized["limit"] = request.limit();
        return true;
    }
    case QUERY_GROUP_CHAT_HISTORY_MSG:
    {
        chat::QueryGroupChatHistoryRequest request;
        if (!request.ParseFromString(payload)) return false;
        normalized["groupid"] = request.groupid();
        normalized["limit"] = request.limit();
        return true;
    }
    case MESSAGE_ACK:
    {
        chat::MessageAck request;
        if (!request.ParseFromString(payload) || request.delivery_id() == 0)
            return false;
        normalized["delivery_id"] = std::to_string(request.delivery_id());
        return true;
    }
    default:
        return false;
    }
}

DecodeResult normalizedProtobuf(std::string frame)
{
    chat::Envelope wireEnvelope;
    if (!wireEnvelope.ParseFromString(frame))
    {
        return DecodeResult::failure(DecodeError::MalformedFrame,
                                     "invalid Protobuf Envelope");
    }

    const int messageType = wireEnvelope.msgid();
    nlohmann::json payload;

    // Qt 兼容路径把原 JSON 对象放在 wire Envelope.payload；Python/C++
    // legacy 路径则使用具体 Protobuf 请求，两者在此统一。
    if (!tryJsonObject(wireEnvelope.payload(), payload))
    {
        payload = nlohmann::json::object();
        if (!normalizeTypedProtobuf(messageType, wireEnvelope.payload(), payload))
        {
            return DecodeResult::failure(
                DecodeError::MalformedFrame,
                "unsupported or malformed Protobuf request payload");
        }
    }

    payload["msgid"] = messageType;
    std::optional<std::uint64_t> deliveryId;
    if (messageType == MESSAGE_ACK)
    {
        deliveryId = gateway::parseDeliveryId(payload);
        if (!deliveryId)
        {
            return DecodeResult::failure(DecodeError::MalformedFrame,
                                         "invalid ACK delivery_id");
        }
    }
    return DecodeResult::complete(gateway::MessageEnvelope(
        messageType, std::move(payload), WireProtocol::Protobuf, deliveryId));
}

WireProtocol detectProtocol(const muduo::net::Buffer& buffer)
{
    const char* data = buffer.peek();
    const std::size_t size = buffer.readableBytes();
    std::size_t position = 0;
    while (position < size &&
           std::isspace(static_cast<unsigned char>(data[position])))
    {
        ++position;
    }

    if (position == size)
    {
        return WireProtocol::Undetermined;
    }
    if (data[position] == '{')
    {
        return WireProtocol::Json;
    }
    if (static_cast<unsigned char>(data[0]) == 0xFF)
    {
        return WireProtocol::Binary;
    }
    return WireProtocol::Protobuf;
}

} // namespace

DecodeResult MessageDecoder::decode(muduo::net::Buffer* buffer,
                                    ProtocolState& protocolState,
                                    std::size_t maxFrameSize)
{
    if (buffer == nullptr)
    {
        return DecodeResult::failure(DecodeError::MalformedFrame,
                                     "null input buffer");
    }
    if (buffer->readableBytes() == 0)
    {
        return DecodeResult::incomplete();
    }

    WireProtocol selected = protocolState.selected();
    if (selected == WireProtocol::Undetermined)
    {
        selected = detectProtocol(*buffer);
        if (selected == WireProtocol::Undetermined)
        {
            if (buffer->readableBytes() > maxFrameSize)
            {
                buffer->retrieveAll();
                return DecodeResult::failure(DecodeError::OversizedFrame,
                                             "undetermined frame exceeds limit");
            }
            return DecodeResult::incomplete();
        }
    }

    DecodeResult decoded;
    switch (selected)
    {
    case WireProtocol::Json:
    {
        auto frame = JsonCodec::decode(buffer, maxFrameSize);
        if (frame.status == FrameDecodeStatus::Incomplete)
            return DecodeResult::incomplete();
        if (frame.status == FrameDecodeStatus::Error)
            return frameFailure(frame);
        decoded = normalizedJson(std::move(frame.frame), WireProtocol::Json);
        break;
    }
    case WireProtocol::Protobuf:
    {
        auto frame = ProtoCodec::decode(buffer, maxFrameSize);
        if (frame.status == FrameDecodeStatus::Incomplete)
            return DecodeResult::incomplete();
        if (frame.status == FrameDecodeStatus::Error)
            return frameFailure(frame);
        decoded = normalizedProtobuf(std::move(frame.frame));
        break;
    }
    case WireProtocol::Binary:
    {
        auto binary = BinaryCodec::decode(buffer, maxFrameSize);
        if (binary.frameResult.status == FrameDecodeStatus::Incomplete)
            return DecodeResult::incomplete();
        if (binary.frameResult.status == FrameDecodeStatus::Error)
            return frameFailure(binary.frameResult);
        decoded = normalizedJson(std::move(binary.frameResult.frame),
                                 WireProtocol::Binary,
                                 binary.messageType);
        break;
    }
    case WireProtocol::Undetermined:
        return DecodeResult::failure(DecodeError::UnsupportedProtocol,
                                     "protocol remains undetermined");
    }

    if (decoded.status == DecodeStatus::Complete &&
        !protocolState.selectDetected(selected))
    {
        return DecodeResult::failure(DecodeError::UnsupportedProtocol,
                                     "wire protocol changed on fixed connection");
    }
    return decoded;
}

} // namespace protocol
