#ifndef CHATSERVER_H
#define CHATSERVER_H

#include <muduo/net/TcpServer.h>
#include <muduo/net/EventLoop.h>
#include "gateway/message_router.hpp"
#include "protocol/protocol_state.hpp"
#include <string>
using namespace muduo;
using namespace muduo::net;

// 聊天服务器的主类
class ChatServer
{
public:
    ChatServer(EventLoop *loop,
               const InetAddress &listenAddr,
               const string &nameArg,
               int threadNum);
    void start();

    // 客户端统一出站边界：业务 JSON 转内部消息后按连接协议编码。
    static void sendResponse(const TcpConnectionPtr& conn,
                             const nlohmann::json& response,
                             bool forceJson = false);
    static void sendResponse(const TcpConnectionPtr& conn,
                             const gateway::MessageEnvelope& response,
                             bool forceJson = false);

private:
    void onConnection(const TcpConnectionPtr &);
    void onMessage(const TcpConnectionPtr &, Buffer *, Timestamp);

    TcpServer _server;
    EventLoop *_loop;
    gateway::MessageRouter _router;
};

#endif

