#ifndef PROTOCOL_MESSAGE_ENCODER_HPP
#define PROTOCOL_MESSAGE_ENCODER_HPP

#include "gateway/message_envelope.hpp"

#include <string>

namespace protocol
{

class MessageEncoder final
{
public:
    // Binary 客户端沿用历史 JSON 响应格式。
    static std::string encode(const gateway::MessageEnvelope& message,
                              WireProtocol protocol);
};

} // namespace protocol

#endif
