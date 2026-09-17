#include <gtest/gtest.h>

#include "binary_codec.hpp"
#include "chat.pb.h"
#include "jsoncodec.hpp"
#include "protocol/message_decoder.hpp"
#include "protocol/message_encoder.hpp"
#include "protocodec.hpp"
#include "public.hpp"

#include <arpa/inet.h>
#include <cstdint>
#include <string>

using muduo::net::Buffer;

namespace
{

std::string typedProtoFrame(int messageType,
                            const google::protobuf::MessageLite& request)
{
    chat::Envelope envelope;
    envelope.set_msgid(messageType);
    envelope.set_payload(request.SerializeAsString());
    return ProtoCodec::encode(envelope.SerializeAsString());
}

std::string jsonPayloadProtoFrame(int messageType, const std::string& payload)
{
    chat::Envelope envelope;
    envelope.set_msgid(messageType);
    envelope.set_payload(payload);
    return ProtoCodec::encode(envelope.SerializeAsString());
}

} // namespace

TEST(MessageDecoderTest, JsonPartialFrameKeepsBytesAndProtocolUndetermined)
{
    Buffer buffer;
    buffer.append("{\"msgid\":4");
    protocol::ProtocolState state;

    auto result = protocol::MessageDecoder::decode(&buffer, state);

    EXPECT_EQ(result.status, protocol::DecodeStatus::Incomplete);
    EXPECT_EQ(state.selected(), protocol::WireProtocol::Undetermined);
    EXPECT_EQ(buffer.readableBytes(), 10u);
}

TEST(MessageDecoderTest, JsonMultipleFramesNormalizeToEnvelope)
{
    Buffer buffer;
    buffer.append(JsonCodec::encode("{\"msgid\":4,\"name\":\"a\"}") +
                  JsonCodec::encode("{\"msgid\":3,\"id\":7}"));
    protocol::ProtocolState state;

    auto first = protocol::MessageDecoder::decode(&buffer, state);
    ASSERT_EQ(first.status, protocol::DecodeStatus::Complete);
    ASSERT_TRUE(first.message.has_value());
    EXPECT_EQ(first.message->messageType(), REG_MSG);
    EXPECT_EQ(first.message->payload()["name"], "a");
    EXPECT_EQ(state.selected(), protocol::WireProtocol::Json);

    auto second = protocol::MessageDecoder::decode(&buffer, state);
    ASSERT_EQ(second.status, protocol::DecodeStatus::Complete);
    EXPECT_EQ(second.message->messageType(), LOGINOUT_MSG);
    EXPECT_EQ(second.message->payload()["id"], 7);
    EXPECT_EQ(buffer.readableBytes(), 0u);
}

TEST(MessageDecoderTest, ProtobufPartialHeaderAndBodyRemainIncomplete)
{
    chat::RegisterRequest request;
    request.set_name("alice");
    request.set_password("secret");
    const std::string encoded = typedProtoFrame(REG_MSG, request);

    Buffer buffer;
    protocol::ProtocolState state;
    buffer.append(encoded.data(), 2);
    auto header = protocol::MessageDecoder::decode(&buffer, state);
    EXPECT_EQ(header.status, protocol::DecodeStatus::Incomplete);
    EXPECT_EQ(state.selected(), protocol::WireProtocol::Undetermined);

    buffer.append(encoded.data() + 2, 4);
    auto body = protocol::MessageDecoder::decode(&buffer, state);
    EXPECT_EQ(body.status, protocol::DecodeStatus::Incomplete);
    EXPECT_EQ(state.selected(), protocol::WireProtocol::Undetermined);

    buffer.append(encoded.data() + 6, encoded.size() - 6);
    auto complete = protocol::MessageDecoder::decode(&buffer, state);
    ASSERT_EQ(complete.status, protocol::DecodeStatus::Complete);
    EXPECT_EQ(complete.message->payload()["name"], "alice");
    EXPECT_EQ(state.selected(), protocol::WireProtocol::Protobuf);
}

TEST(MessageDecoderTest, ProtobufMultipleFramesAreExtractedIndividually)
{
    chat::LoginRequest firstRequest;
    firstRequest.set_id(1);
    firstRequest.set_password("one");
    chat::LoginRequest secondRequest;
    secondRequest.set_id(2);
    secondRequest.set_password("two");

    Buffer buffer;
    buffer.append(typedProtoFrame(LOGIN_MSG, firstRequest) +
                  typedProtoFrame(LOGIN_MSG, secondRequest));
    protocol::ProtocolState state;

    auto first = protocol::MessageDecoder::decode(&buffer, state);
    auto second = protocol::MessageDecoder::decode(&buffer, state);

    ASSERT_EQ(first.status, protocol::DecodeStatus::Complete);
    ASSERT_EQ(second.status, protocol::DecodeStatus::Complete);
    EXPECT_EQ(first.message->payload()["id"], 1);
    EXPECT_EQ(second.message->payload()["id"], 2);
    EXPECT_EQ(buffer.readableBytes(), 0u);
}

TEST(MessageDecoderTest, JsonAndProtobufOversizedFramesAreRejected)
{
    Buffer jsonBuffer;
    jsonBuffer.append("{\"msgid\":4,\"padding\":\"xxxxxxxx\"}");
    protocol::ProtocolState jsonState;
    auto jsonResult = protocol::MessageDecoder::decode(&jsonBuffer, jsonState, 16);
    EXPECT_EQ(jsonResult.status, protocol::DecodeStatus::Error);
    EXPECT_EQ(jsonResult.error, protocol::DecodeError::OversizedFrame);

    const uint32_t networkLength = htonl(65u);
    Buffer protoBuffer;
    protoBuffer.append(reinterpret_cast<const char*>(&networkLength),
                       sizeof(networkLength));
    protocol::ProtocolState protoState;
    auto protoResult =
        protocol::MessageDecoder::decode(&protoBuffer, protoState, 64);
    EXPECT_EQ(protoResult.status, protocol::DecodeStatus::Error);
    EXPECT_EQ(protoResult.error, protocol::DecodeError::OversizedFrame);
}

TEST(MessageDecoderTest, MalformedJsonAndProtobufReturnError)
{
    Buffer jsonBuffer;
    const std::string malformedJson = JsonCodec::encode("{not-json}");
    jsonBuffer.append(malformedJson);
    protocol::ProtocolState jsonState;
    auto jsonResult = protocol::MessageDecoder::decode(&jsonBuffer, jsonState);
    EXPECT_EQ(jsonResult.status, protocol::DecodeStatus::Error);
    EXPECT_EQ(jsonResult.error, protocol::DecodeError::MalformedFrame);

    Buffer protoBuffer;
    protoBuffer.append(ProtoCodec::encode(std::string(1, '\xFF')));
    protocol::ProtocolState protoState;
    auto protoResult = protocol::MessageDecoder::decode(&protoBuffer, protoState);
    EXPECT_EQ(protoResult.status, protocol::DecodeStatus::Error);
    EXPECT_EQ(protoResult.error, protocol::DecodeError::MalformedFrame);
}

TEST(MessageDecoderTest, DetectedProtocolIsRetainedUntilExplicitNegotiation)
{
    Buffer buffer;
    buffer.append(JsonCodec::encode("{\"msgid\":4,\"name\":\"a\"}"));
    protocol::ProtocolState state;
    ASSERT_EQ(protocol::MessageDecoder::decode(&buffer, state).status,
              protocol::DecodeStatus::Complete);
    ASSERT_EQ(state.selected(), protocol::WireProtocol::Json);

    chat::RegisterRequest request;
    request.set_name("protobuf");
    request.set_password("secret");
    const std::string proto = typedProtoFrame(REG_MSG, request);
    buffer.append(proto);

    auto fixed = protocol::MessageDecoder::decode(&buffer, state);
    EXPECT_EQ(fixed.status, protocol::DecodeStatus::Error);
    EXPECT_EQ(fixed.error, protocol::DecodeError::MalformedFrame);
    EXPECT_EQ(state.selected(), protocol::WireProtocol::Json);

    buffer.retrieveAll();
    state.selectAfterNegotiation(protocol::WireProtocol::Protobuf);
    buffer.append(proto);
    auto switched = protocol::MessageDecoder::decode(&buffer, state);
    ASSERT_EQ(switched.status, protocol::DecodeStatus::Complete);
    EXPECT_EQ(switched.message->payload()["name"], "protobuf");
}

TEST(MessageDecoderTest, ProtobufAcceptsQtJsonPayloadCompatibilityPath)
{
    Buffer buffer;
    buffer.append(jsonPayloadProtoFrame(
        ADD_FRIEND_MSG, "{\"msgid\":999,\"id\":1,\"friendid\":2}"));
    protocol::ProtocolState state;

    auto result = protocol::MessageDecoder::decode(&buffer, state);

    ASSERT_EQ(result.status, protocol::DecodeStatus::Complete);
    EXPECT_EQ(result.message->messageType(), ADD_FRIEND_MSG);
    EXPECT_EQ(result.message->payload()["msgid"], ADD_FRIEND_MSG);
    EXPECT_EQ(result.message->payload()["friendid"], 2);
}

TEST(MessageDecoderTest, BinaryBodyUsesHeaderMessageType)
{
    Buffer buffer;
    buffer.append(BinaryCodec::encode(ADD_GROUP_MSG, "{\"id\":1,\"groupid\":2}"));
    protocol::ProtocolState state;

    auto result = protocol::MessageDecoder::decode(&buffer, state);

    ASSERT_EQ(result.status, protocol::DecodeStatus::Complete);
    EXPECT_EQ(result.message->messageType(), ADD_GROUP_MSG);
    EXPECT_EQ(result.message->payload()["msgid"], ADD_GROUP_MSG);
    EXPECT_EQ(state.selected(), protocol::WireProtocol::Binary);
}

TEST(MessageDecoderTest, SplitAndCoalescedFramesPreserveOrder)
{
    const auto first = JsonCodec::encode("{\"msgid\":4,\"name\":\"first\"}");
    const auto second = JsonCodec::encode("{\"msgid\":4,\"name\":\"second\"}");
    const auto third = JsonCodec::encode("{\"msgid\":4,\"name\":\"third\"}");
    Buffer buffer;
    protocol::ProtocolState state;
    buffer.append(first + second.substr(0, 6));

    auto result1 = protocol::MessageDecoder::decode(&buffer, state);
    ASSERT_EQ(result1.status, protocol::DecodeStatus::Complete);
    EXPECT_EQ(result1.message->payload()["name"], "first");
    EXPECT_EQ(protocol::MessageDecoder::decode(&buffer, state).status,
              protocol::DecodeStatus::Incomplete);

    buffer.append(second.substr(6) + third);
    auto result2 = protocol::MessageDecoder::decode(&buffer, state);
    auto result3 = protocol::MessageDecoder::decode(&buffer, state);
    ASSERT_EQ(result2.status, protocol::DecodeStatus::Complete);
    ASSERT_EQ(result3.status, protocol::DecodeStatus::Complete);
    EXPECT_EQ(result2.message->payload()["name"], "second");
    EXPECT_EQ(result3.message->payload()["name"], "third");
    EXPECT_EQ(buffer.readableBytes(), 0u);
}

TEST(MessageDecoderTest, JsonAndTypedProtobufProduceSameBusinessEnvelope)
{
    Buffer jsonBuffer;
    jsonBuffer.append(JsonCodec::encode(
        "{\"msgid\":4,\"name\":\"alice\",\"password\":\"secret\"}"));
    protocol::ProtocolState jsonState;
    auto jsonResult = protocol::MessageDecoder::decode(&jsonBuffer, jsonState);

    chat::RegisterRequest request;
    request.set_name("alice");
    request.set_password("secret");
    Buffer protoBuffer;
    protoBuffer.append(typedProtoFrame(REG_MSG, request));
    protocol::ProtocolState protoState;
    auto protoResult = protocol::MessageDecoder::decode(&protoBuffer, protoState);

    ASSERT_EQ(jsonResult.status, protocol::DecodeStatus::Complete);
    ASSERT_EQ(protoResult.status, protocol::DecodeStatus::Complete);
    EXPECT_EQ(jsonResult.message->messageType(),
              protoResult.message->messageType());
    EXPECT_EQ(jsonResult.message->payload(), protoResult.message->payload());
    EXPECT_EQ(jsonResult.message->sourceProtocol(),
              protocol::WireProtocol::Json);
    EXPECT_EQ(protoResult.message->sourceProtocol(),
              protocol::WireProtocol::Protobuf);
}

TEST(MessageDecoderTest, ProtobufSplitAndCoalescedFramesPreserveOrder)
{
    chat::LoginRequest firstRequest;
    firstRequest.set_id(1);
    firstRequest.set_password("one");
    chat::LoginRequest secondRequest;
    secondRequest.set_id(2);
    secondRequest.set_password("two");
    chat::LoginRequest thirdRequest;
    thirdRequest.set_id(3);
    thirdRequest.set_password("three");
    const auto first = typedProtoFrame(LOGIN_MSG, firstRequest);
    const auto second = typedProtoFrame(LOGIN_MSG, secondRequest);
    const auto third = typedProtoFrame(LOGIN_MSG, thirdRequest);

    Buffer buffer;
    protocol::ProtocolState state;
    buffer.append(first + second.substr(0, 2));
    auto result1 = protocol::MessageDecoder::decode(&buffer, state);
    ASSERT_EQ(result1.status, protocol::DecodeStatus::Complete);
    EXPECT_EQ(result1.message->payload()["id"], 1);
    EXPECT_EQ(protocol::MessageDecoder::decode(&buffer, state).status,
              protocol::DecodeStatus::Incomplete);

    buffer.append(second.substr(2, 4));
    EXPECT_EQ(protocol::MessageDecoder::decode(&buffer, state).status,
              protocol::DecodeStatus::Incomplete);
    buffer.append(second.substr(6) + third);
    auto result2 = protocol::MessageDecoder::decode(&buffer, state);
    auto result3 = protocol::MessageDecoder::decode(&buffer, state);
    ASSERT_EQ(result2.status, protocol::DecodeStatus::Complete);
    ASSERT_EQ(result3.status, protocol::DecodeStatus::Complete);
    EXPECT_EQ(result2.message->payload()["id"], 2);
    EXPECT_EQ(result3.message->payload()["id"], 3);
    EXPECT_EQ(buffer.readableBytes(), 0u);
}

TEST(MessageDecoderTest, JsonAndProtobufAckCarrySameDeliveryId)
{
    const std::uint64_t id = 18446744073709551614ull;
    Buffer jsonBuffer;
    jsonBuffer.append(JsonCodec::encode(
        R"({"msgid":20,"delivery_id":"18446744073709551614"})"));
    protocol::ProtocolState jsonState;
    auto jsonAck = protocol::MessageDecoder::decode(&jsonBuffer, jsonState);

    chat::MessageAck request;
    request.set_delivery_id(id);
    Buffer protoBuffer;
    protoBuffer.append(typedProtoFrame(MESSAGE_ACK, request));
    protocol::ProtocolState protoState;
    auto protoAck = protocol::MessageDecoder::decode(&protoBuffer, protoState);

    ASSERT_EQ(jsonAck.status, protocol::DecodeStatus::Complete);
    ASSERT_EQ(protoAck.status, protocol::DecodeStatus::Complete);
    EXPECT_EQ(jsonAck.message->messageType(), MESSAGE_ACK);
    EXPECT_EQ(protoAck.message->messageType(), MESSAGE_ACK);
    EXPECT_EQ(jsonAck.message->deliveryId(), id);
    EXPECT_EQ(protoAck.message->deliveryId(), id);
    EXPECT_EQ(jsonAck.message->payload(), protoAck.message->payload());
}

TEST(MessageDecoderTest, RejectsInvalidAckIdWithoutThrowing)
{
    for (const std::string body : {
             R"({"msgid":20,"delivery_id":"0"})",
             R"({"msgid":20,"delivery_id":42})",
             R"({"msgid":20,"delivery_id":"18446744073709551616"})",
             R"({"msgid":20,"delivery_id":"-1"})"})
    {
        Buffer buffer;
        buffer.append(JsonCodec::encode(body));
        protocol::ProtocolState state;
        EXPECT_EQ(protocol::MessageDecoder::decode(&buffer, state).status,
                  protocol::DecodeStatus::Error);
    }
    chat::MessageAck invalid;
    invalid.set_delivery_id(0);
    Buffer buffer;
    buffer.append(typedProtoFrame(MESSAGE_ACK, invalid));
    protocol::ProtocolState state;
    EXPECT_EQ(protocol::MessageDecoder::decode(&buffer, state).status,
              protocol::DecodeStatus::Error);
}

TEST(MessageDecoderTest, ReliableDeliveryEncodesForJsonAndProtobuf)
{
    gateway::MessageEnvelope delivery(
        ONE_CHAT_MSG, {{"msgid", ONE_CHAT_MSG}, {"msg", "hello"}},
        protocol::WireProtocol::Undetermined, 123456789ull);

    Buffer jsonBuffer;
    jsonBuffer.append(protocol::MessageEncoder::encode(
        delivery, protocol::WireProtocol::Json));
    protocol::ProtocolState jsonState;
    auto jsonResult = protocol::MessageDecoder::decode(&jsonBuffer, jsonState);
    ASSERT_EQ(jsonResult.status, protocol::DecodeStatus::Complete);
    EXPECT_EQ(jsonResult.message->payload()["delivery_id"], "123456789");

    Buffer protoBuffer;
    protoBuffer.append(protocol::MessageEncoder::encode(
        delivery, protocol::WireProtocol::Protobuf));
    std::string wire;
    ASSERT_TRUE(ProtoCodec::tryExtractFrame(&protoBuffer, wire));
    chat::Envelope envelope;
    ASSERT_TRUE(envelope.ParseFromString(wire));
    EXPECT_EQ(envelope.msgid(), ONE_CHAT_MSG);
    auto payload = nlohmann::json::parse(envelope.payload());
    EXPECT_EQ(payload["delivery_id"], "123456789");
}

TEST(MessageDecoderTest, AckEnvelopeEncodesThroughBothWireCodecs)
{
    gateway::MessageEnvelope ack(
        MESSAGE_ACK, {{"msgid", MESSAGE_ACK}},
        protocol::WireProtocol::Undetermined, 987654321ull);
    for (auto wireProtocol : {protocol::WireProtocol::Json,
                              protocol::WireProtocol::Protobuf})
    {
        Buffer buffer;
        buffer.append(protocol::MessageEncoder::encode(ack, wireProtocol));
        protocol::ProtocolState state;
        auto result = protocol::MessageDecoder::decode(&buffer, state);
        ASSERT_EQ(result.status, protocol::DecodeStatus::Complete);
        EXPECT_EQ(result.message->messageType(), MESSAGE_ACK);
        EXPECT_EQ(result.message->deliveryId(), 987654321ull);
    }
}
