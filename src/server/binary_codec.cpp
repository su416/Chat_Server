#include "binary_codec.hpp"
#include <cstring>
#include <arpa/inet.h>

using muduo::net::Buffer;

std::string BinaryCodec::encode(int msgId, const std::string& body)
{
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
    // 至少需要 4 字节头部（0xFF + msgId + 2B len）
    if (buffer->readableBytes() < 4)
        return false;

    const char* p = buffer->peek();
    if (static_cast<unsigned char>(p[0]) != 0xFF)
        return false;

    msgId = static_cast<unsigned char>(p[1]);

    uint16_t netLen;
    memcpy(&netLen, p + 2, sizeof(netLen));
    uint16_t bodyLen = ntohs(netLen);

    if (buffer->readableBytes() < 4u + bodyLen)
        return false;

    buffer->retrieve(4);
    body.assign(buffer->peek(), bodyLen);
    buffer->retrieve(bodyLen);
    return true;
}
