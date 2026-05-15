#include "protocodec.hpp"
#include <cstring>
#include <arpa/inet.h>

using muduo::net::Buffer;

std::string ProtoCodec::encode(const std::string& payload)
{
    uint32_t len = static_cast<uint32_t>(payload.size());
    uint32_t netLen = htonl(len);

    std::string data;
    data.append(reinterpret_cast<const char*>(&netLen), sizeof(netLen));
    data.append(payload);
    return data;
}

bool ProtoCodec::tryExtractFrame(Buffer* buffer, std::string& frame)
{
    // 至少需要 4 字节长度前缀
    if (buffer->readableBytes() < sizeof(uint32_t))
        return false;

    // 读取长度前缀（Big-Endian）
    uint32_t netLen = 0;
    memcpy(&netLen, buffer->peek(), sizeof(netLen));
    uint32_t len = ntohl(netLen);

    // 检查完整消息是否已到齐
    if (buffer->readableBytes() < sizeof(uint32_t) + len)
        return false;

    // 跳过长度前缀，读取消息体
    buffer->retrieve(sizeof(uint32_t));
    frame.assign(buffer->peek(), len);
    buffer->retrieve(len);
    return true;
}
