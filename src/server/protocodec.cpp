#include "protocodec.hpp"
#include <cstring>
#include <arpa/inet.h>
#include <stdexcept>
#include <utility>

using muduo::net::Buffer;

std::string ProtoCodec::encode(const std::string& payload)
{
    if (payload.size() > protocol::kDefaultMaxFrameSize)
    {
        throw std::length_error("protobuf frame exceeds configured limit");
    }
    uint32_t len = static_cast<uint32_t>(payload.size());
    uint32_t netLen = htonl(len);

    std::string data;
    data.append(reinterpret_cast<const char*>(&netLen), sizeof(netLen));
    data.append(payload);
    return data;
}

bool ProtoCodec::tryExtractFrame(Buffer* buffer, std::string& frame)
{
    auto result = decode(buffer);
    if (result.status != protocol::FrameDecodeStatus::Complete)
    {
        return false;
    }
    frame = std::move(result.frame);
    return true;
}

protocol::FrameDecodeResult ProtoCodec::decode(
    Buffer* buffer,
    std::size_t maxFrameSize)
{
    if (buffer == nullptr)
    {
        return protocol::FrameDecodeResult::failure(
            protocol::FrameDecodeError::MalformedFrame);
    }

    // 至少需要 4 字节长度前缀
    if (buffer->readableBytes() < sizeof(uint32_t))
        return protocol::FrameDecodeResult::incomplete();

    // 读取长度前缀（Big-Endian）
    uint32_t netLen = 0;
    memcpy(&netLen, buffer->peek(), sizeof(netLen));
    uint32_t len = ntohl(netLen);

    if (len > maxFrameSize)
    {
        buffer->retrieveAll();
        return protocol::FrameDecodeResult::failure(
            protocol::FrameDecodeError::OversizedFrame);
    }

    // 检查完整消息是否已到齐
    if (buffer->readableBytes() < sizeof(uint32_t) +
                                      static_cast<std::size_t>(len))
        return protocol::FrameDecodeResult::incomplete();

    // 跳过长度前缀，读取消息体
    buffer->retrieve(sizeof(uint32_t));
    std::string frame(buffer->peek(), len);
    buffer->retrieve(len);
    return protocol::FrameDecodeResult::complete(std::move(frame));
}
