#ifndef BINARY_CODEC_HPP
#define BINARY_CODEC_HPP

#include <string>
#include <muduo/net/Buffer.h>

// Binary 协议编解码
// 帧格式：[1B 0xFF][1B msgId][2B bodyLen BE][body bytes]
class BinaryCodec
{
public:
    // 编码：返回完整帧 bytes
    static std::string encode(int msgId, const std::string& body);

    // 解码：从 Buffer 中提取一帧。返回 true 表示成功拿到完整帧
    static bool tryExtractFrame(muduo::net::Buffer* buffer, int& msgId, std::string& body);
};

#endif
