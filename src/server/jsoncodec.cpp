#include "server/jsoncodec.hpp"

#include <algorithm>
#include <utility>

using muduo::net::Buffer;

std::string JsonCodec::encode(const std::string& payload)
{
    std::string data = payload;
    data.push_back('\0');
    return data;
}

bool JsonCodec::tryExtractFrame(Buffer* buffer, std::string& frame)
{
    auto result = decode(buffer);
    if (result.status != protocol::FrameDecodeStatus::Complete)
    {
        return false;
    }
    frame = std::move(result.frame);
    return true;
}

protocol::FrameDecodeResult JsonCodec::decode(
    Buffer* buffer,
    std::size_t maxFrameSize)
{
    if (buffer == nullptr)
    {
        return protocol::FrameDecodeResult::failure(
            protocol::FrameDecodeError::MalformedFrame);
    }

    const char* begin = buffer->peek();
    const char* end = std::find(begin, begin + buffer->readableBytes(), '\0');

    if (end == begin + buffer->readableBytes())
    {
        if (buffer->readableBytes() > maxFrameSize)
        {
            buffer->retrieveAll();
            return protocol::FrameDecodeResult::failure(
                protocol::FrameDecodeError::OversizedFrame);
        }
        return protocol::FrameDecodeResult::incomplete();
    }

    const auto frameSize = static_cast<std::size_t>(end - begin);
    if (frameSize > maxFrameSize)
    {
        buffer->retrieveUntil(end + 1);
        return protocol::FrameDecodeResult::failure(
            protocol::FrameDecodeError::OversizedFrame);
    }

    std::string frame(begin, end);
    buffer->retrieveUntil(end + 1);
    return protocol::FrameDecodeResult::complete(std::move(frame));
}
