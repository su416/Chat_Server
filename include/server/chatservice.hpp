#ifndef CHATSERVICE_H
#define CHATSERVICE_H

#include <muduo/net/TcpConnection.h>
#include <unordered_map>
#include <functional>

using namespace std;
using namespace muduo;
using namespace muduo::net;

#include "redis.hpp"
#include "groupmodel.hpp"
#include "friendmodel.hpp"
#include "usermodel.hpp"
#include "chathistorymodel.hpp"
#include "gateway/session_manager.hpp"
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
    MsgHandler getHandler(int msgid);
    // 从 redis 消息队列中获取订阅的消息
    void handleRedisSubscribeMessage(int, string);

    // 同步连接数到 Redis（供负载均衡）
    void syncServerConnections();

    // 查询最近单聊历史消息
    void queryOneChatHistory(const TcpConnectionPtr &conn, json &js, Timestamp time);

    // 查询最近群聊历史消息
    void queryGroupChatHistory(const TcpConnectionPtr &conn, json &js, Timestamp time);

private:
    ChatService();

    // 存储消息 id 和其对应的业务处理方法
    unordered_map<int, MsgHandler> _msgHandlerMap;
    // 逻辑会话与 TCP 连接生命周期分离；业务层不再直接维护连接映射。
    gateway::SessionManager _sessionManager;

    // 数据操作类对象
    UserModel _userModel;
    FriendModel _friendModel;
    GroupModel _groupModel;
    ChatHistoryModel _chatHistoryModel;

    // redis 操作对象
    Redis _redis;
};

#endif
