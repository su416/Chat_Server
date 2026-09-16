#ifndef PROTOCOL_FRAME_DECODE_RESULT_HPP
#define PROTOCOL_FRAME_DECODE_RESULT_HPP

#include <cstddef>
#include <string>
#include <utility>

namespace protocol
{

constexpr std::size_t kDefaultMaxFrameSize = 1024U * 1024U;

enum class FrameDecodeStatus
{
    Incomplete,
    Complete,
    Error
};

enum class FrameDecodeError
{
    None,
    OversizedFrame,
    MalformedFrame
};

struct FrameDecodeResult
{
    FrameDecodeStatus status = FrameDecodeStatus::Incomplete;
    FrameDecodeError error = FrameDecodeError::None;
    std::string frame;

    static FrameDecodeResult incomplete()
    {
        return {};
    }

    static FrameDecodeResult complete(std::string value)
    {
        return {FrameDecodeStatus::Complete,
                FrameDecodeError::None,
                std::move(value)};
    }

    static FrameDecodeResult failure(FrameDecodeError reason)
    {
        return {FrameDecodeStatus::Error, reason, {}};
    }
};

} // namespace protocol

#endif
