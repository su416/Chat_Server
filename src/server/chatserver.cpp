#include "chatserver.hpp"
#include "json.hpp"
#include "chatservice.hpp"
#include "server/jsoncodec.hpp"

#include <muduo/base/Logging.h>
#include <iostream>
#include <functional>
#include <string>

using namespace std;
using namespace placeholders;
using json = nlohmann::json;

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
    if (!conn->connected())
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

    while (JsonCodec::tryExtractFrame(buffer, frame))
    {
        if (frame.empty())
        {
            continue;
        }

        try
        {
            json js = json::parse(frame);

            auto msgHandler = ChatService::instance()->getHandler(js["msgid"].get<int>());
            msgHandler(conn, js, time);
        }
        catch (const std::exception &e)
        {
            LOG_ERROR << "invalid json frame: " << e.what()
                      << " frame=" << frame;
        }
    }
}