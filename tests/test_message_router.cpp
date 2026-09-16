#include <gtest/gtest.h>

#include "gateway/message_router.hpp"
#include "protocol/message_encoder.hpp"
#include "protocodec.hpp"
#include "chat.pb.h"

#include <stdexcept>

TEST(MessageRouterTest, DispatchesEnvelopeThroughLegacyHandlerAdapter)
{
    int dispatches = 0;
    gateway::MessageRouter router([&](int messageType)
        -> std::optional<MsgHandler> {
        if (messageType != 6) return std::nullopt;
        return MsgHandler([&](const TcpConnectionPtr&, json& request, Timestamp) {
            EXPECT_EQ(request["msg"], "hello");
            ++dispatches;
        });
    });

    gateway::MessageEnvelope message(
        6, json{{"msgid", 6}, {"msg", "hello"}},
        protocol::WireProtocol::Json);
    EXPECT_EQ(router.dispatch({}, message, Timestamp::now()),
              gateway::DispatchResult::Handled);
    EXPECT_EQ(dispatches, 1);
}

TEST(MessageRouterTest, UnknownTypeAndHandlerExceptionAreContained)
{
    gateway::MessageRouter router([](int messageType)
        -> std::optional<MsgHandler> {
        if (messageType == 999) return std::nullopt;
        return MsgHandler([](const TcpConnectionPtr&, json&, Timestamp) {
            throw std::runtime_error("invalid handler input");
        });
    });
    gateway::MessageEnvelope unknown(999, json{{"msgid", 999}},
                                     protocol::WireProtocol::Json);
    gateway::MessageEnvelope invalid(6, json{{"msgid", 6}},
                                     protocol::WireProtocol::Json);

    EXPECT_EQ(router.dispatch({}, unknown, Timestamp::now()),
              gateway::DispatchResult::UnknownMessageType);
    EXPECT_EQ(router.dispatch({}, invalid, Timestamp::now()),
              gateway::DispatchResult::HandlerError);
}

TEST(MessageEncoderTest, JsonAndProtobufResponsesHaveExistingWireFraming)
{
    gateway::MessageEnvelope response(
        5, json{{"msgid", 5}, {"errno", 0}, {"id", 42}},
        protocol::WireProtocol::Json);

    const auto jsonWire = protocol::MessageEncoder::encode(
        response, protocol::WireProtocol::Json);
    EXPECT_EQ(jsonWire.back(), '\0');
    EXPECT_EQ(json::parse(jsonWire.substr(0, jsonWire.size() - 1))["id"], 42);

    const auto protoWire = protocol::MessageEncoder::encode(
        response, protocol::WireProtocol::Protobuf);
    muduo::net::Buffer buffer;
    buffer.append(protoWire);
    std::string frame;
    ASSERT_TRUE(ProtoCodec::tryExtractFrame(&buffer, frame));
    chat::Envelope wireEnvelope;
    ASSERT_TRUE(wireEnvelope.ParseFromString(frame));
    EXPECT_EQ(wireEnvelope.msgid(), 5);
    EXPECT_EQ(wireEnvelope.errcode(), 0);
    EXPECT_EQ(json::parse(wireEnvelope.payload())["id"], 42);
}
