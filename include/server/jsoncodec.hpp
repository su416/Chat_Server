#ifndef JSONCODEC_H
#define JSONCODEC_H

#include <muduo/net/Buffer.h>

#include "protocol/frame_decode_result.hpp"

#include <cstddef>
#include <string>

class JsonCodec
{
public:
    // 给 TCP 发送数据时追加 '\0' 分隔符
    static std::string encode(const std::string& payload);

    // 兼容旧调用：只在取得完整帧时返回 true。
    static bool tryExtractFrame(muduo::net::Buffer* buffer, std::string& frame);

    // 显式区分半包、完整帧和错误，并对未终止输入执行大小限制。
    static protocol::FrameDecodeResult decode(
        muduo::net::Buffer* buffer,
        std::size_t maxFrameSize = protocol::kDefaultMaxFrameSize);
};

#endif
