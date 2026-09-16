#ifndef PROTOCOL_MESSAGE_DECODER_HPP
#define PROTOCOL_MESSAGE_DECODER_HPP

#include "gateway/message_envelope.hpp"
#include "protocol/frame_decode_result.hpp"
#include "protocol/protocol_state.hpp"

#include <muduo/net/Buffer.h>

#include <optional>
#include <string>
#include <utility>

namespace protocol
{

enum class DecodeStatus
{
    Incomplete,
    Complete,
    Error
};

enum class DecodeError
{
    None,
    OversizedFrame,
    MalformedFrame,
    UnsupportedProtocol
};

struct DecodeResult
{
    DecodeStatus status = DecodeStatus::Incomplete;
    DecodeError error = DecodeError::None;
    std::optional<gateway::MessageEnvelope> message;
    std::string detail;

    static DecodeResult incomplete()
    {
        return {};
    }

    static DecodeResult complete(gateway::MessageEnvelope value)
    {
        DecodeResult result;
        result.status = DecodeStatus::Complete;
        result.message.emplace(std::move(value));
        return result;
    }

    static DecodeResult failure(DecodeError reason, std::string description)
    {
        DecodeResult result;
        result.status = DecodeStatus::Error;
        result.error = reason;
        result.detail = std::move(description);
        return result;
    }
};

class MessageDecoder final
{
public:
    static DecodeResult decode(
        muduo::net::Buffer* buffer,
        ProtocolState& protocolState,
        std::size_t maxFrameSize = kDefaultMaxFrameSize);
};

} // namespace protocol

#endif
