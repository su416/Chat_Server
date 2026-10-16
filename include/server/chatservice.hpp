#ifndef CHATSERVICE_H
#define CHATSERVICE_H

#include <muduo/net/TcpConnection.h>
#include <muduo/net/EventLoop.h>
#include <unordered_map>
#include <functional>
#include <optional>
#include <atomic>
#include <mutex>
#include <unordered_set>
#include <deque>
#include <thread>
#include <condition_variable>

using namespace std;
using namespace muduo;
using namespace muduo::net;

#include "redis.hpp"
#include "groupmodel.hpp"
#include "friendmodel.hpp"
#include "usermodel.hpp"
#include "chathistorymodel.hpp"
#include "gateway/session_manager.hpp"
#include "gateway/pending_delivery_manager.hpp"
#include "model/durablependingmodel.hpp"
#include "cluster/presence_manager.hpp"
#include "cluster/route_transport.hpp"
#include "json.hpp"
using json = nlohmann::json;

// 表示处理消息的事件回调方法类型
using MsgHandler = std::function<void(const TcpConnectionPtr &conn, json &js, Timestamp)>;

// 聊天服务器业务类
class ChatService
{
public:
    // 获取单例对象的接口函数
    static ChatService *instance();
    // 获取 Redis 实例（供 main 注册服务器）
    Redis* getRedis() { return &_redis; }
    // 协议协商（客户端声明偏好，服务端确认并存储到 conn context）
    void negotiate(const TcpConnectionPtr &conn, json &js, Timestamp time);
    // 处理登录业务
    void login(const TcpConnectionPtr &conn, json &js, Timestamp time);
    // 处理注册业务
    void reg(const TcpConnectionPtr &conn, json &js, Timestamp time);
    // 一对一聊天业务
    void oneChat(const TcpConnectionPtr &conn, json &js, Timestamp time);
    void messageAck(const TcpConnectionPtr &conn, json &js, Timestamp time);
    void scanReliableDeliveries();
    void logDeliveryMetrics() const;
    void heartbeatPresence();
    void startHeartbeat();
    void logClusterMetrics() const;
    void setMainLoop(muduo::net::EventLoop* loop) { _mainLoop.store(loop); }
    // 添加好友业务
    void addFriend(const TcpConnectionPtr &conn, json &js, Timestamp time);
    // 创建群组业务
    void createGroup(const TcpConnectionPtr &conn, json &js, Timestamp time);
    // 加入群组业务
    void addGroup(const TcpConnectionPtr &conn, json &js, Timestamp time);
    // 群组聊天业务
    void groupChat(const TcpConnectionPtr &conn, json &js, Timestamp time);
    // 处理注销业务
    void loginout(const TcpConnectionPtr &conn, json &js, Timestamp time);
    // 处理客户端异常退出
    void clientCloseException(const TcpConnectionPtr &conn);
    // 服务器异常，业务重置方法
    void reset();
    // 获取消息对应的处理器
    std::optional<MsgHandler> findHandler(int msgid) const;
    // 从 redis 消息队列中获取订阅的消息
    cluster::RouteOutcome handleRoutedMessage(cluster::RoutedMessage message);

    // 同步连接数到 Redis（供负载均衡）
    void syncServerConnections();

    // 查询最近单聊历史消息
    void queryOneChatHistory(const TcpConnectionPtr &conn, json &js, Timestamp time);

    // 查询最近群聊历史消息
    void queryGroupChatHistory(const TcpConnectionPtr &conn, json &js, Timestamp time);

private:
    ChatService();
    enum class RouteStatus { Sent, Offline, TargetOverloaded, Error };
    RouteStatus routeToUser(int receiverId, int senderId, int messageType,
                            const json& payload);
    void processRoutedMessage(
        cluster::RoutedMessage message,
        std::function<void(cluster::RouteOutcome)> complete);
    void persistAndDeliver(
        const std::shared_ptr<const gateway::Session>& session,
        const json& payload, const std::string& routeId,
        std::function<void(bool)> complete);
    bool activateDurable(
        const std::shared_ptr<const gateway::Session>& session,
        const gateway::DurablePendingRecord& record);
    void recoverForSession(const std::shared_ptr<const gateway::Session>& session);
    void scanInboxRecovery();
    void enqueueInbox(int userId);
    void submitDeferredAck(std::uint64_t deliveryId, int userId);
    void scanDeferredAcks();
    bool hasDeferredAck(std::uint64_t deliveryId) const;
    void storeDurableOffline(int senderId, int receiverId, const json& payload,
                             const TcpConnectionPtr& sender,
                             std::function<void()> accepted);
    void recoverAtStartup();
    void handleDueDurableRecord(gateway::DurablePendingRecord record);
    void storeLegacyGroupOffline(int senderId, int receiverId, const json& payload);

    // 存储消息 id 和其对应的业务处理方法
    unordered_map<int, MsgHandler> _msgHandlerMap;
    // 逻辑会话与 TCP 连接生命周期分离；业务层不再直接维护连接映射。
    gateway::SessionManager _sessionManager;
    gateway::PendingDeliveryManager _pendingDeliveries;
    gateway::DurablePendingModel _durablePendingModel;

    // 数据操作类对象
    UserModel _userModel;
    FriendModel _friendModel;
    GroupModel _groupModel;
    ChatHistoryModel _chatHistoryModel;

    // redis 操作对象
    Redis _redis;
    cluster::PresenceManager _presence;
    cluster::RouteTransport _route;
    std::atomic<std::uint64_t> _routeLocal{0}, _routeRemote{0},
        _routeReceived{0}, _routeStale{0}, _routeFailed{0};
    std::atomic<muduo::net::EventLoop*> _mainLoop{nullptr};
    std::atomic<bool> _durableScanInFlight{false};
    std::atomic<std::uint64_t> _durablePersisted{0}, _durableRecovered{0},
        _durableAcked{0}, _durableDbFailed{0};
    mutable std::mutex _inboxMutex;
    std::unordered_set<int> _inboxQueued, _inboxInFlight;
    std::deque<int> _inboxOrder;
    std::atomic<std::uint64_t> _waitingCreated{0}, _waitingClaimed{0},
        _waitingRequeued{0}, _offlineRecovered{0}, _legacyMigrated{0},
        _legacyFailed{0};
    std::atomic<std::uint64_t> _durableExpired{0};
    std::atomic<int> _waitingCursor{0};
    std::atomic<std::size_t> _dbPendingCount{0}, _dbWaitingCount{0};
    mutable std::mutex _ackMutex;
    std::unordered_map<std::uint64_t, int> _deferredAcks;
    std::unordered_set<std::uint64_t> _ackInFlight;
    std::atomic<std::uint64_t> _ackDeferredTotal{0};
    std::atomic<std::uint64_t> _ackDbFailures{0};
    std::atomic<std::uint64_t> _historyRejected{0};
    std::thread _heartbeatWorker;
    std::mutex _heartbeatMutex;
    std::condition_variable _heartbeatCv;
    bool _heartbeatStop = false;
};

#endif
