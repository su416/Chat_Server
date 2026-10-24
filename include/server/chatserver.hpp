#ifndef CHATSERVER_H
#define CHATSERVER_H

#include <muduo/net/TcpServer.h>
#include <muduo/net/EventLoop.h>
#include "gateway/message_router.hpp"
#include "protocol/protocol_state.hpp"
#include <string>
#include <atomic>
#include <mutex>
#include <unordered_map>
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
    static bool isBackpressured(const TcpConnectionPtr& conn);
    struct OutputMetrics {
        std::uint64_t activeConnections, slowConsumers;
        std::uint64_t highwaterEvents, hardDisconnects;
        std::uint64_t queuedPeakBytes, outputPeakBytes;
    };
    static OutputMetrics outputMetrics();
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
    static std::mutex _flowMutex;
    struct FlowState {
        bool slow = false, closeQueued = false;
        std::size_t queuedBytes = 0;
    };
    static std::unordered_map<const TcpConnection*, FlowState> _connectionFlow;
    static std::atomic<std::uint64_t> _activeConnections, _slowConsumers,
        _highwaterEvents, _hardDisconnects, _queuedPeakBytes, _outputPeakBytes;
    static void markSlow(const TcpConnectionPtr& conn, bool slow);
    enum class ReserveOutcome { Accepted, Close, AlreadyClosing };
    static ReserveOutcome reserveQueued(const TcpConnectionPtr& conn, std::size_t bytes);
    static void releaseQueued(const TcpConnectionPtr& conn, std::size_t bytes);
};

#endif

