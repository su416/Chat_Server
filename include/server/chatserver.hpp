#ifndef CHATSERVER_H
#define CHATSERVER_H

#include <muduo/net/TcpServer.h>
#include <muduo/net/EventLoop.h>
#include <string>
using namespace muduo;
using namespace muduo::net;

// 连接协议类型（通过 conn->setContext 存储）
enum ConnProtocol { PROTO_JSON = 1, PROTO_PROTOBUF = 2 };

// 聊天服务器的主类
class ChatServer
{
public:
    ChatServer(EventLoop *loop,
               const InetAddress &listenAddr,
               const string &nameArg,
               int threadNum);
    void start();

    // 协议感知响应编码：JSON 连接走 JsonCodec，Protobuf 连接走 ProtoCodec
    static std::string encodeResponse(const TcpConnectionPtr& conn,
                                      const std::string& jsonBody);

private:
    void onConnection(const TcpConnectionPtr &);
    void onMessage(const TcpConnectionPtr &, Buffer *, Timestamp);

    TcpServer _server;
    EventLoop *_loop;
};

#endif

