#include "server/jsoncodec.hpp"

#include <algorithm>

using muduo::net::Buffer;

std::string JsonCodec::encode(const std::string &payload)
{
    std::string data = payload;
    data.push_back('\0');
    return data;
}

bool JsonCodec::tryExtractFrame(Buffer *buffer, std::string &frame)
{
    const char *begin = buffer->peek();
    const char *end = std::find(begin, begin + buffer->readableBytes(), '\0');

    if (end == begin + buffer->readableBytes())
    {
        return false; // 当前没有完整帧，等下次继续收
    }

    frame.assign(begin, end);
    buffer->retrieveUntil(end + 1); // 连 '\0' 一起取走
    return true;
}
