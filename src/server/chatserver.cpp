#include "chatserver.hpp"
#include "json.hpp"
#include "chatservice.hpp"
#include "protocol/message_decoder.hpp"
#include "protocol/message_encoder.hpp"
#include "protocol/protocol_state.hpp"

#include <boost/any.hpp>
#include <muduo/base/Logging.h>
#include <functional>
#include <stdexcept>
#include <string>

using namespace std;
using namespace placeholders;
using json = nlohmann::json;

void ChatServer::sendResponse(const TcpConnectionPtr& conn,
                              const json& response,
                              bool forceJson)
{
    try
    {
        if (!response.is_object())
        {
            throw std::invalid_argument("outbound response must be JSON object");
        }
        gateway::MessageEnvelope message(response.value("msgid", 0), response,
                                         protocol::WireProtocol::Undetermined);
        sendResponse(conn, message, forceJson);
    }
    catch (const std::exception& error)
    {
        LOG_WARN << "unable to encode client response: " << error.what();
        conn->shutdown();
    }
}

void ChatServer::sendResponse(const TcpConnectionPtr& conn,
                              const gateway::MessageEnvelope& response,
                              bool forceJson)
{
    if (!conn) return;
    // Redis callback、别的连接的 I/O loop、主 loop 定时器都可能调用本入口。
    // 只在目标连接的 EventLoop 读取 ProtocolState，并且不延长断线连接寿命。
    std::weak_ptr<TcpConnection> weakConnection = conn;
    conn->getLoop()->runInLoop([weakConnection, response, forceJson]() {
        auto target = weakConnection.lock();
        if (!target || !target->connected()) return;
        try
        {
            protocol::WireProtocol selected = protocol::WireProtocol::Json;
            if (!forceJson)
            {
                const auto& context = target->getContext();
                if (const auto* state =
                        boost::any_cast<protocol::ProtocolState>(&context))
                {
                    selected = state->selected();
                }
            }
            target->send(protocol::MessageEncoder::encode(response, selected));
        }
        catch (const std::exception& error)
        {
            LOG_WARN << "unable to encode client response: " << error.what();
            target->shutdown();
        }
    });
}

// 初始化聊天服务器对象
ChatServer::ChatServer(EventLoop *loop,
                       const InetAddress &listenAddr,
                       const string &nameArg,
                       int threadNum)
    : _server(loop, listenAddr, nameArg),
      _loop(loop),
      _router([](int messageType) {
          return ChatService::instance()->findHandler(messageType);
      })
{
    _server.setConnectionCallback(std::bind(&ChatServer::onConnection, this, _1));
    _server.setMessageCallback(std::bind(&ChatServer::onMessage, this, _1, _2, _3));
    _server.setThreadNum(threadNum);
}

void ChatServer::start()
{
    _server.start();
}

void ChatServer::onConnection(const TcpConnectionPtr &conn)
{
    if (conn->connected())
    {
        conn->setContext(protocol::ProtocolState{});
    }
    else
    {
        ChatService::instance()->clientCloseException(conn);
        conn->shutdown();
    }
}

void ChatServer::onMessage(const TcpConnectionPtr &conn,
                           Buffer *buffer,
                           Timestamp time)
{
    while (buffer->readableBytes() > 0)
    {
        protocol::ProtocolState state;
        const auto& context = conn->getContext();
        if (const auto* stored =
                boost::any_cast<protocol::ProtocolState>(&context))
        {
            state = *stored;
        }

        auto decoded = protocol::MessageDecoder::decode(buffer, state);
        if (decoded.status == protocol::DecodeStatus::Incomplete)
        {
            break;
        }
        if (decoded.status == protocol::DecodeStatus::Error)
        {
            LOG_WARN << "protocol decode rejected connection: "
                     << decoded.detail;
            buffer->retrieveAll();
            conn->shutdown();
            return;
        }

        conn->setContext(state);
        const auto dispatch = _router.dispatch(conn, *decoded.message, time);
        if (dispatch != gateway::DispatchResult::Handled)
        {
            LOG_WARN << "message dispatch rejected type="
                     << decoded.message->messageType();
            buffer->retrieveAll();
            conn->shutdown();
            return;
        }
    }
}
