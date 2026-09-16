#ifndef PROTOCODEC_HPP
#define PROTOCODEC_HPP

#include <muduo/net/Buffer.h>
#include "protocol/frame_decode_result.hpp"

#include <cstddef>
#include <string>

// Protobuf 长度前缀分帧编解码器
// 格式：[4-byte big-endian length][protobuf bytes]
class ProtoCodec
{
public:
    // 编码：添加 4 字节 Big-Endian 长度前缀
    static std::string encode(const std::string& payload);

    // 从 Buffer 中提取一帧完整数据，成功返回 true
    // 数据不完整时返回 false，保留半包在 buffer 中
    static bool tryExtractFrame(muduo::net::Buffer* buffer, std::string& frame);

    static protocol::FrameDecodeResult decode(
        muduo::net::Buffer* buffer,
        std::size_t maxFrameSize = protocol::kDefaultMaxFrameSize);
};

#endif
