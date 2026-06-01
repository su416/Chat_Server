// 测试模块：JsonCodec（JSON \0 分帧编解码）
// 覆盖：正常帧、多帧、空帧、不完整帧、编解码往返

#include <gtest/gtest.h>
#include "server/jsoncodec.hpp"
#include <muduo/net/Buffer.h>

using muduo::net::Buffer;

TEST(JsonCodecTest, EncodeAppendsNull)
{
    std::string data = JsonCodec::encode("hello");
    ASSERT_EQ(data.size(), 6u);
    EXPECT_EQ(data[5], '\0');
    EXPECT_EQ(std::string(data.data(), 5), "hello");
}

TEST(JsonCodecTest, ExtractSingleFrame)
{
    Buffer buf;
    buf.append("{\"a\":1}\0", 8);

    std::string frame;
    EXPECT_TRUE(JsonCodec::tryExtractFrame(&buf, frame));
    EXPECT_EQ(frame, "{\"a\":1}");
    EXPECT_EQ(buf.readableBytes(), 0u);
}

TEST(JsonCodecTest, ExtractMultipleFrames)
{
    Buffer buf;
    buf.append("a\0b\0", 4);

    std::string frame1, frame2;
    EXPECT_TRUE(JsonCodec::tryExtractFrame(&buf, frame1));
    EXPECT_EQ(frame1, "a");

    EXPECT_TRUE(JsonCodec::tryExtractFrame(&buf, frame2));
    EXPECT_EQ(frame2, "b");

    EXPECT_EQ(buf.readableBytes(), 0u);
}

TEST(JsonCodecTest, IncompleteFrameReturnsFalse)
{
    Buffer buf;
    buf.append("abc");  // 没有 \0

    std::string frame;
    EXPECT_FALSE(JsonCodec::tryExtractFrame(&buf, frame));
    EXPECT_EQ(buf.readableBytes(), 3u);  // 数据未被消费
}

TEST(JsonCodecTest, EmptyFrameSkipped)
{
    Buffer buf;
    buf.append("\0", 1);

    std::string frame;
    EXPECT_TRUE(JsonCodec::tryExtractFrame(&buf, frame));
    EXPECT_EQ(frame, "");
}

TEST(JsonCodecTest, PartialThenComplete)
{
    Buffer buf;
    buf.append("hel");

    std::string frame;
    EXPECT_FALSE(JsonCodec::tryExtractFrame(&buf, frame));

    // 剩余数据到达
    buf.append("lo\0", 3);
    EXPECT_TRUE(JsonCodec::tryExtractFrame(&buf, frame));
    EXPECT_EQ(frame, "hello");
}
