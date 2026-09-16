#include "binary_codec.hpp"
#include <cstring>
#include <arpa/inet.h>
#include <limits>
#include <stdexcept>
#include <utility>

using muduo::net::Buffer;

std::string BinaryCodec::encode(int msgId, const std::string& body)
{
    if (body.size() > std::numeric_limits<uint16_t>::max())
    {
        throw std::length_error("binary frame body exceeds uint16 length");
    }

    // 格式：[1B 0xFF][1B msgId][2B bodyLen BE][body]
    std::string data;
    data.push_back('\xFF');
    data.push_back(static_cast<char>(msgId));

    uint16_t netLen = htons(static_cast<uint16_t>(body.size()));
    data.append(reinterpret_cast<const char*>(&netLen), sizeof(netLen));
    data.append(body);
    return data;
}

bool BinaryCodec::tryExtractFrame(Buffer* buffer, int& msgId, std::string& body)
{
    auto result = decode(buffer);
    if (result.frameResult.status != protocol::FrameDecodeStatus::Complete)
    {
        return false;
    }
    msgId = result.messageType;
    body = std::move(result.frameResult.frame);
    return true;
}

BinaryFrameDecodeResult BinaryCodec::decode(
    Buffer* buffer,
    std::size_t maxFrameSize)
{
    if (buffer == nullptr)
    {
        return {protocol::FrameDecodeResult::failure(
                    protocol::FrameDecodeError::MalformedFrame),
                0};
    }

    // 至少需要 4 字节头部（0xFF + msgId + 2B len）
    if (buffer->readableBytes() < 4)
        return {protocol::FrameDecodeResult::incomplete(), 0};

    const char* p = buffer->peek();
    if (static_cast<unsigned char>(p[0]) != 0xFF)
    {
        buffer->retrieveAll();
        return {protocol::FrameDecodeResult::failure(
                    protocol::FrameDecodeError::MalformedFrame),
                0};
    }

    const int msgId = static_cast<unsigned char>(p[1]);

    uint16_t netLen;
    memcpy(&netLen, p + 2, sizeof(netLen));
    uint16_t bodyLen = ntohs(netLen);

    if (bodyLen > maxFrameSize)
    {
        buffer->retrieveAll();
        return {protocol::FrameDecodeResult::failure(
                    protocol::FrameDecodeError::OversizedFrame),
                msgId};
    }

    if (buffer->readableBytes() < 4u + bodyLen)
        return {protocol::FrameDecodeResult::incomplete(), msgId};

    buffer->retrieve(4);
    std::string body(buffer->peek(), bodyLen);
    buffer->retrieve(bodyLen);
    return {protocol::FrameDecodeResult::complete(std::move(body)), msgId};
}
