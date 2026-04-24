#ifndef JSONCODEC_H
#define JSONCODEC_H

#include <muduo/net/Buffer.h>
#include <string>

class JsonCodec
{
public:
    // 给 TCP 发送数据时追加 '\0' 分隔符
    static std::string encode(const std::string &payload);

    // 从 muduo::Buffer 中提取一条完整消息（以 '\0' 结尾）
    // 如果当前没有完整帧，返回 false，并保留半包在 buffer 中
    static bool tryExtractFrame(muduo::net::Buffer *buffer, std::string &frame);
};

#endif
