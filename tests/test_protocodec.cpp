// 测试模块：ProtoCodec（Protobuf 长度前缀分帧）
// 覆盖：编解码往返、空 payload、不完整帧、大 payload

#include <gtest/gtest.h>
#include "protocodec.hpp"
#include <muduo/net/Buffer.h>
#include <arpa/inet.h>

using muduo::net::Buffer;

TEST(ProtoCodecTest, EncodeDecodeRoundtrip)
{
    std::string payload = "hello protobuf";
    std::string encoded = ProtoCodec::encode(payload);

    // 编码后：4 字节长度 + payload
    ASSERT_EQ(encoded.size(), 4 + payload.size());

    Buffer buf;
    buf.append(encoded);

    std::string decoded;
    EXPECT_TRUE(ProtoCodec::tryExtractFrame(&buf, decoded));
    EXPECT_EQ(decoded, payload);
}

TEST(ProtoCodecTest, EmptyPayload)
{
    std::string encoded = ProtoCodec::encode("");
    ASSERT_EQ(encoded.size(), 4u);

    Buffer buf;
    buf.append(encoded);

    std::string decoded;
    EXPECT_TRUE(ProtoCodec::tryExtractFrame(&buf, decoded));
    EXPECT_EQ(decoded, "");
}

TEST(ProtoCodecTest, IncompleteLengthPrefix)
{
    Buffer buf;
    buf.append("\x00\x00", 2);  // 只到了 2 字节，长度前缀不完整

    std::string frame;
    EXPECT_FALSE(ProtoCodec::tryExtractFrame(&buf, frame));
    EXPECT_EQ(buf.readableBytes(), 2u);
}

TEST(ProtoCodecTest, IncompletePayload)
{
    std::string encoded = ProtoCodec::encode("long message");
    Buffer buf;
    buf.append(encoded.data(), 8);  // 4 字节长度 + 4 字节 payload，缺后面

    std::string frame;
    EXPECT_FALSE(ProtoCodec::tryExtractFrame(&buf, frame));
    EXPECT_EQ(buf.readableBytes(), 8u);
}

TEST(ProtoCodecTest, LargePayload)
{
    std::string payload(10000, 'x');
    std::string encoded = ProtoCodec::encode(payload);

    Buffer buf;
    buf.append(encoded);

    std::string decoded;
    EXPECT_TRUE(ProtoCodec::tryExtractFrame(&buf, decoded));
    EXPECT_EQ(decoded, payload);
}

TEST(ProtoCodecTest, MultipleFrames)
{
    std::string e1 = ProtoCodec::encode("first");
    std::string e2 = ProtoCodec::encode("second");

    Buffer buf;
    buf.append(e1 + e2);

    std::string d1, d2;
    EXPECT_TRUE(ProtoCodec::tryExtractFrame(&buf, d1));
    EXPECT_EQ(d1, "first");
    EXPECT_TRUE(ProtoCodec::tryExtractFrame(&buf, d2));
    EXPECT_EQ(d2, "second");
}

TEST(ProtoCodecTest, OversizedLengthIsRejectedBeforeBodyArrives)
{
    uint32_t length = htonl(65u);
    Buffer buffer;
    buffer.append(reinterpret_cast<const char*>(&length), sizeof(length));

    auto result = ProtoCodec::decode(&buffer, 64);

    EXPECT_EQ(result.status, protocol::FrameDecodeStatus::Error);
    EXPECT_EQ(result.error, protocol::FrameDecodeError::OversizedFrame);
    EXPECT_EQ(buffer.readableBytes(), 0u);
}
