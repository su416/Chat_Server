#include "chatserver.hpp"
#include "json.hpp"
#include "chatservice.hpp"
#include "server/jsoncodec.hpp"
#include "protocodec.hpp"
#include "binary_codec.hpp"
#include "chat.pb.h"

#include <boost/any.hpp>
#include <muduo/base/Logging.h>
#include <iostream>
#include <functional>
#include <string>

using namespace std;
using namespace placeholders;
using json = nlohmann::json;

namespace
{

// Protobuf → JSON 桥接：根据 msgid 解析 payload 并构造等效 JSON
json protoToJson(int msgid, const string& payload)
{
    json js;
    js["msgid"] = msgid;

    switch (msgid)
    {
    case 1: // LOGIN_MSG
    {
        chat::LoginRequest req;
        if (req.ParseFromString(payload))
        {
            js["id"] = req.id();
            js["password"] = req.password();
        }
        break;
    }
    case 4: // REG_MSG
    {
        chat::RegisterRequest req;
        if (req.ParseFromString(payload))
        {
            js["name"] = req.name();
            js["password"] = req.password();
        }
        break;
    }
    case 6: // ONE_CHAT_MSG
    {
        chat::OneChatRequest req;
        if (req.ParseFromString(payload))
        {
            js["id"] = req.id();
            js["toid"] = req.toid();
            js["name"] = req.name();
            js["msg"] = req.msg();
            js["time"] = req.time();
        }
        break;
    }
    default:
        break;
    }
    return js;
}

// JSON 响应 → Protobuf 包装（作为响应 Envelope 的 payload）
string jsonToProtoResponse(int msgid, int errcode, const string& errmsg,
                           const json& response)
{
    chat::Envelope env;
    env.set_msgid(msgid);
    env.set_errcode(errcode);
    env.set_errmsg(errmsg);
    // 响应原样返回 JSON 字符串作为 payload
    env.set_payload(response.dump());
    return env.SerializeAsString();
}

} // namespace

// 协议感知响应编码
std::string ChatServer::encodeResponse(const TcpConnectionPtr& conn,
                                       const std::string& jsonBody)
{
    const auto& ctx = conn->getContext();
    const ConnProtocol* proto = boost::any_cast<ConnProtocol>(&ctx);
    if (proto && *proto == ConnProtocol::PROTO_PROTOBUF)
    {
        // Protobuf 连接 → 用 Envelope 包装 + ProtoCodec 分帧
        chat::Envelope env;
        try {
            json js = json::parse(jsonBody);
            env.set_msgid(js.value("msgid", 0));
            env.set_errcode(js.value("errno", 0));
        } catch (...) {}
        env.set_payload(jsonBody);
        return ProtoCodec::encode(env.SerializeAsString());
    }

    // JSON 连接（或未设置 context）→ JsonCodec 分帧
    return JsonCodec::encode(jsonBody);
}

// 初始化聊天服务器对象
ChatServer::ChatServer(EventLoop *loop,
                       const InetAddress &listenAddr,
                       const string &nameArg,
                       int threadNum)
    : _server(loop, listenAddr, nameArg), _loop(loop)
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
        // 新连接默认按 JSON 协议处理
        conn->setContext(ConnProtocol::PROTO_JSON);
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
    std::string frame;

    while (buffer->readableBytes() > 0)
    {
        // 协议检测：JSON 以 '{' 开头，Protobuf 首字节是二进制长度前缀
        char firstByte = buffer->peek()[0];

        if (firstByte == '{')
        {
            // ──── JSON 协议路径 ────
            conn->setContext(ConnProtocol::PROTO_JSON);

            if (!JsonCodec::tryExtractFrame(buffer, frame))
                break;

            if (frame.empty())
                continue;

            try
            {
                json js = json::parse(frame);
                auto msgHandler = ChatService::instance()->getHandler(
                    js["msgid"].get<int>());
                msgHandler(conn, js, time);
            }
            catch (const std::exception &e)
            {
                LOG_ERROR << "invalid json frame: " << e.what()
                          << " frame=" << frame;
            }
        }
        else if (static_cast<unsigned char>(firstByte) == 0xFF)
        {
            // ──── Binary 协议路径（0xFF 标记头） ────
            int msgId;
            if (!BinaryCodec::tryExtractFrame(buffer, msgId, frame))
                break;

            try
            {
                json js = json::parse(frame);
                auto msgHandler = ChatService::instance()->getHandler(msgId);
                msgHandler(conn, js, time);
            }
            catch (const std::exception &e)
            {
                LOG_ERROR << "binary frame parse error: " << e.what();
            }
        }
        else
        {
            // ──── Protobuf 协议路径 ────
            conn->setContext(ConnProtocol::PROTO_PROTOBUF);

            if (!ProtoCodec::tryExtractFrame(buffer, frame))
                break;

            try
            {
                chat::Envelope env;
                if (!env.ParseFromString(frame))
                {
                    LOG_ERROR << "protobuf: failed to parse envelope";
                    continue;
                }

                int msgid = env.msgid();
                json js = protoToJson(msgid, env.payload());

                auto msgHandler = ChatService::instance()->getHandler(msgid);
                msgHandler(conn, js, time);
            }
            catch (const std::exception &e)
            {
                LOG_ERROR << "protobuf frame error: " << e.what();
            }
        }
    }
}