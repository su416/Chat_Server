#include "chatservice.hpp"
#include "chatserver.hpp"
#include "public.hpp"
#include "password_utils.hpp"
#include "rate_limiter.hpp"
#include "db_thread_pool.hpp"
#include "connectionpool.h"
#include <muduo/base/Logging.h>
#include <vector>
#include "proto_utils.hpp"
#include "config.hpp"
#include "gateway/delivery_id.hpp"
#include <boost/any.hpp>
#include <csignal>
#include <cstdlib>
#include <future>
#include <algorithm>
#include <chrono>

using namespace std;
using namespace muduo;

namespace
{
gateway::DeliveryPolicy deliveryPolicy()
{
    gateway::DeliveryPolicy policy;
    if (const char* value = std::getenv("CHAT_PHASE7_TEST_MAX_PENDING")) {
        try {
            const auto max = std::stoul(value);
            if (max > 0 && max <= 10000) policy.maxPending = max;
        } catch (...) { /* keep production default */ }
    }
    return policy;
}

void sendTcpJson(const TcpConnectionPtr &conn, const json &js)
{
    ChatServer::sendResponse(conn, js);
}

}

// 获取单例对象的接口函数
ChatService *ChatService::instance()
{
    static ChatService service;
    return &service;
}

// 注册消息以及对应的Handler回调操作
ChatService::ChatService()
    : _pendingDeliveries(deliveryPolicy()),
      _presence(_redis, Config::instance().getNodeId()),
      _route(_redis, Config::instance().getNodeId(), _presence.generation())
{
    // 协议协商（msgid=0）——客户端声明偏好，服务端确认
    _msgHandlerMap.insert({NEGOTIATE_MSG, std::bind(&ChatService::negotiate, this, _1, _2, _3)});

    // 用户基本业务管理相关事件处理回调注册
    _msgHandlerMap.insert({LOGIN_MSG, std::bind(&ChatService::login, this, _1, _2, _3)});
    _msgHandlerMap.insert({LOGINOUT_MSG, std::bind(&ChatService::loginout, this, _1, _2, _3)});
    _msgHandlerMap.insert({REG_MSG, std::bind(&ChatService::reg, this, _1, _2, _3)});
    _msgHandlerMap.insert({ONE_CHAT_MSG, std::bind(&ChatService::oneChat, this, _1, _2, _3)});
    _msgHandlerMap.insert({MESSAGE_ACK, std::bind(&ChatService::messageAck, this, _1, _2, _3)});
    _msgHandlerMap.insert({ADD_FRIEND_MSG, std::bind(&ChatService::addFriend, this, _1, _2, _3)});

    // 群组业务管理相关事件处理回调注册
    _msgHandlerMap.insert({CREATE_GROUP_MSG, std::bind(&ChatService::createGroup, this, _1, _2, _3)});
    _msgHandlerMap.insert({ADD_GROUP_MSG, std::bind(&ChatService::addGroup, this, _1, _2, _3)});
    _msgHandlerMap.insert({GROUP_CHAT_MSG, std::bind(&ChatService::groupChat, this, _1, _2, _3)});

    // 查询历史消息
    _msgHandlerMap.insert({QUERY_ONE_CHAT_HISTORY_MSG, std::bind(&ChatService::queryOneChatHistory, this, _1, _2, _3)});
    _msgHandlerMap.insert({QUERY_GROUP_CHAT_HISTORY_MSG, std::bind(&ChatService::queryGroupChatHistory, this, _1, _2, _3)});

    if (!_redis.connect()) LOG_ERROR << "Redis unavailable at startup";
    if (!_presence.registerNode()) LOG_ERROR << "NODE_REGISTER failed";
    if (!_route.start([this](cluster::RoutedMessage message) {
            return handleRoutedMessage(std::move(message));
        })) LOG_ERROR << "ROUTE_STREAM_CONSUMER failed";
    recoverAtStartup();
}

// 服务器异常，业务重置方法
void ChatService::reset()
{
    {
        std::lock_guard<std::mutex> lock(_heartbeatMutex);
        _heartbeatStop = true;
    }
    _heartbeatCv.notify_all();
    if (_heartbeatWorker.joinable()) _heartbeatWorker.join();
    _route.stop();
    _mainLoop.store(nullptr);
    for (const auto& [userId, sessionId] : _sessionManager.activeSessions())
    {
        if (_presence.unbindUser(userId, sessionId))
            _userModel.updateState(User(userId, "", "", "offline"));
    }
    _presence.unregisterNode();
    _redis.stop();
}

// 获取消息对应的处理器；未知类型由 MessageRouter 显式处理。
std::optional<MsgHandler> ChatService::findHandler(int msgid) const
{
    auto it = _msgHandlerMap.find(msgid);
    if (it == _msgHandlerMap.end())
    {
        return std::nullopt;
    }
    return it->second;
}

// 同步当前在线人数到 Redis（供负载均衡选最少连接实例）
void ChatService::syncServerConnections()
{
    auto& cfg = Config::instance();
    int count = static_cast<int>(_sessionManager.activeCount());
    _redis.updateConnections(cfg.getServerName(), count);
}

// 协议协商 msgid=0
void ChatService::negotiate(const TcpConnectionPtr &conn, json &js, Timestamp time)
{
    string accept = js.value("accept", "json");
    json response;
    response["msgid"] = 0;

    protocol::ProtocolState state;
    const auto& context = conn->getContext();
    if (const auto* stored = boost::any_cast<protocol::ProtocolState>(&context))
    {
        state = *stored;
    }

    if (accept == "protobuf")
    {
        state.selectAfterNegotiation(protocol::WireProtocol::Protobuf);
        response["content_type"] = "protobuf";
        response["status"] = "ok";
    }
    else if (accept == "json")
    {
        state.selectAfterNegotiation(protocol::WireProtocol::Json);
        response["content_type"] = "json";
        response["status"] = "ok";
    }
    else
    {
        // 不认识的格式，保持默认 JSON
        state.selectAfterNegotiation(protocol::WireProtocol::Json);
        response["content_type"] = "json";
        response["status"] = "unknown_format, fallback to json";
    }

    conn->setContext(state);

    // 协商响应始终走 JSON（客户端还没切协议）。
    ChatServer::sendResponse(conn, response, true);
}

// 处理登录业务
void ChatService::login(const TcpConnectionPtr &conn, json &js, Timestamp time)
{
    int id = js["id"].get<int>();
    string pwd = js["password"];

    User user = _userModel.query(id);
    if (user.getId() == id && PasswordUtils::verifyPassword(pwd, user.getPwd()))
    {
        // MySQL state 是展示缓存，不是跨节点登录仲裁；Redis bind 是 last-login-wins。
        {
            // 登录成功，绑定逻辑会话。并发登录时只允许第一个绑定成功。
            auto bindOutcome = _sessionManager.bind(id, conn);
            if (!bindOutcome.bound())
            {
                json response;
                response["msgid"] = LOGIN_MSG_ACK;
                response["errno"] = 2;
                response["errmsg"] = "this account is using, input another!";
                sendTcpJson(conn, response);
                return;
            }

            if (!_presence.bindUser(id, bindOutcome.sessionId))
            {
                _sessionManager.unbindUser(id, conn);
                sendTcpJson(conn, json{{"msgid", LOGIN_MSG_ACK}, {"errno", 6},
                                       {"errmsg", "presence unavailable"}});
                return;
            }
            syncServerConnections();

            // 登录成功，更新用户状态信息 state offline=>online
            user.setState("online");
            _userModel.updateState(user);

            json response;
            response["msgid"] = LOGIN_MSG_ACK;
            response["errno"] = 0;
            response["id"] = user.getId();
            response["name"] = user.getName();

            // 查询该用户是否有离线消息（从 chatmessage 表，status=2）
            vector<string> vec = _chatHistoryModel.queryOffline(id);
            if (!vec.empty())
            {
                response["offlinemsg"] = vec;
                // 标记为已送达
                _chatHistoryModel.markDelivered(id);
            }

            // 查询该用户的好友信息并返回
            vector<User> userVec = _friendModel.query(id);
            if (!userVec.empty())
            {
                vector<string> vec2;
                for (User &user : userVec)
                {
                    json js;
                    js["id"] = user.getId();
                    js["name"] = user.getName();
                    js["state"] = user.getState();
                    vec2.push_back(js.dump());
                }
                response["friends"] = vec2;
            }

            // 查询用户的群组信息
            vector<Group> groupuserVec = _groupModel.queryGroups(id);
            if (!groupuserVec.empty())
            {
                vector<string> groupV;
                for (Group &group : groupuserVec)
                {
                    json grpjson;
                    grpjson["id"] = group.getId();
                    grpjson["groupname"] = group.getName();
                    grpjson["groupdesc"] = group.getDesc();

                    vector<string> userV;
                    for (GroupUser &user : group.getUsers())
                    {
                        json js;
                        js["id"] = user.getId();
                        js["name"] = user.getName();
                        js["state"] = user.getState();
                        js["role"] = user.getRole();
                        userV.push_back(js.dump());
                    }

                    grpjson["users"] = userV;
                    groupV.push_back(grpjson.dump());
                }

                response["groups"] = groupV;
            }

            sendTcpJson(conn, response);
            // Login success is independent of inbox size. The main-loop
            // scanner claims a bounded batch asynchronously after this ACK.
            enqueueInbox(id);
        }
    }
    else
    {
        // 该用户不存在，用户存在但是密码错误，登录失败
        json response;
        response["msgid"] = LOGIN_MSG_ACK;
        response["errno"] = 1;
        response["errmsg"] = "id or password is invalid!";
        sendTcpJson(conn, response);
    }
}

// 处理注册业务
void ChatService::reg(const TcpConnectionPtr &conn, json &js, Timestamp time)
{
    string name = js["name"];
    string pwd = js["password"];

    User user;
    user.setName(name);
    user.setPwd(pwd);

    bool state = _userModel.insert(user);
    if (state)
    {
        json response;
        response["msgid"] = REG_MSG_ACK;
        response["errno"] = 0;
        response["id"] = user.getId();
        sendTcpJson(conn, response);
    }
    else
    {
        json response;
        response["msgid"] = REG_MSG_ACK;
        response["errno"] = 1;
        sendTcpJson(conn, response);
    }
}

// 处理注销业务
void ChatService::loginout(const TcpConnectionPtr &conn, json &js, Timestamp time)
{
    int userid = js["id"].get<int>();
    auto session = _sessionManager.sessionForConnection(conn);
    if (!session || session->userId() != userid) return;

    // 只允许当前会话对应的连接解绑；旧连接的迟到请求不能删除新会话。
    if (!_sessionManager.unbindUser(userid, conn))
    {
        return;
    }

    const auto suspended = _pendingDeliveries.suspendConnection(conn);
    if (!suspended.empty())
        LOG_INFO << "DELIVERY_WAITING_SESSION count=" << suspended.size();
    syncServerConnections();

    // 旧节点迟到 logout 不得删除新节点 owner，也不得把 MySQL 展示状态写离线。
    if (_presence.unbindUser(userid, session->id()))
        _userModel.updateState(User(userid, "", "", "offline"));
}

// 处理客户端异常退出
void ChatService::clientCloseException(const TcpConnectionPtr &conn)
{
    const auto suspended = _pendingDeliveries.suspendConnection(conn);
    if (!suspended.empty())
        LOG_INFO << "DELIVERY_WAITING_SESSION count=" << suspended.size();
    auto session = _sessionManager.sessionForConnection(conn);
    auto userId = _sessionManager.unbindConnection(conn);
    if (!userId)
    {
        // 未登录连接断开不触发 Redis 退订或数据库状态更新。
        return;
    }

    syncServerConnections();

    if (session && _presence.unbindUser(*userId, session->id()))
        _userModel.updateState(User(*userId, "", "", "offline"));
}

// 一对一聊天业务
void ChatService::oneChat(const TcpConnectionPtr &conn, json &js, Timestamp time)
{
    // delivery_id 只能由服务端为本地在线投递生成，不能接受发送者自报身份。
    js.erase("delivery_id");
    int fromid = js["id"].get<int>();
    int toid = js["toid"].get<int>();
    string msg = js["msg"];

    auto senderSession = _sessionManager.sessionForConnection(conn);
    if (!senderSession || senderSession->userId() != fromid ||
        !_presence.ownsUser(fromid, senderSession->id()))
    {
        ++_routeStale;
        sendTcpJson(conn, json{{"msgid", ONE_CHAT_MSG_ACK}, {"errno", 6},
                               {"errmsg", "sender is not current owner"}});
        return;
    }

    // 限流检查
    if (!RateLimiter::instance()->allow(fromid))
    {
        json response;
        response["msgid"] = ONE_CHAT_MSG_ACK;
        response["errno"] = 3;
        response["errmsg"] = "rate limit exceeded, slow down";
        sendTcpJson(conn, response);
        return;
    }

    // 异步落库单聊历史消息（快照发送者昵称到 extra 列，避免历史查询 JOIN）
    string name = js.value("name", "");
    string extra = name.empty() ? "" : json{{"name", name}}.dump();
    auto recordHistory = [this, fromid, toid, msg, extra]() {
        if (DBThreadPool::instance()->submitVoid([this, fromid, toid, msg, extra]() {
            _chatHistoryModel.insertOneChat(fromid, toid, msg, 0, extra);
        }) != DBThreadPool::SubmitResult::Accepted) {
            const auto count = ++_historyRejected;
            if (count == 1 || count % 100 == 0)
                LOG_WARN << "HISTORY_QUEUE_REJECTED count=" << count;
        }
    };

    if (auto session = _sessionManager.sessionForUser(toid))
    {
        if (auto targetConnection = session->connection();
            targetConnection && _presence.ownsUser(toid, session->id()))
        {
            ++_routeLocal;
            // 在目标连接的 I/O loop 读取 ProtocolState，避免跨线程访问 context。
            // 旧 Binary 接收端没有 ACK 能力，保留 Phase 2 原投递行为。
            std::weak_ptr<const gateway::Session> weakSession = session;
            std::weak_ptr<TcpConnection> weakTarget = targetConnection;
            std::weak_ptr<TcpConnection> weakSender = conn;
            const json request = js;
            targetConnection->getLoop()->runInLoop(
                [this, weakSession, weakTarget, weakSender, request,
                 toid, recordHistory]() {
                    auto currentSession = weakSession.lock();
                    auto target = weakTarget.lock();
                    if (!currentSession || !target || !target->connected() ||
                        _sessionManager.sessionForUser(toid) != currentSession ||
                        !_presence.ownsUser(toid, currentSession->id()))
                    {
                        if (auto sender = weakSender.lock())
                        {
                            sendTcpJson(sender, json{{"msgid", ONE_CHAT_MSG_ACK},
                                                     {"errno", 5},
                                                     {"errmsg", "receiver session changed"}});
                        }
                        return;
                    }

                    const auto& context = target->getContext();
                    const auto* state = boost::any_cast<protocol::ProtocolState>(&context);
                    if (state && state->selected() == protocol::WireProtocol::Binary)
                    {
                        recordHistory();
                        sendTcpJson(target, request);
                        return;
                    }

                    persistAndDeliver(currentSession, request, "",
                        [weakSender, recordHistory](bool accepted) {
                            if (accepted) { recordHistory(); return; }
                            if (auto sender = weakSender.lock())
                                sendTcpJson(sender, json{{"msgid", ONE_CHAT_MSG_ACK},
                                    {"errno", 4},
                                    {"errmsg", "durable delivery unavailable"}});
                        });
                });
            return;
        }
    }

    const auto status = routeToUser(toid, fromid, ONE_CHAT_MSG, js);
    if (status == RouteStatus::Offline)
        storeDurableOffline(fromid, toid, js, conn, recordHistory);
    else if (status == RouteStatus::Sent) recordHistory();
    else if (status == RouteStatus::Error)
        sendTcpJson(conn, json{{"msgid", ONE_CHAT_MSG_ACK}, {"errno", 6},
                               {"errmsg", "cross-node route unavailable"}});
}

// 添加好友业务
void ChatService::addFriend(const TcpConnectionPtr &conn, json &js, Timestamp time)
{
    int userid = js["id"].get<int>();
    int friendid = js["friendid"].get<int>();

    auto sender = _sessionManager.sessionForConnection(conn);
    if (!sender || sender->userId() != userid ||
        !_presence.ownsUser(userid, sender->id()))
    {
        ++_routeStale;
        sendTcpJson(conn, json{{"msgid", ADD_FRIEND_MSG_ACK}, {"errno", 6},
                               {"errmsg", "sender is not current owner"}});
        return;
    }

    json response;
    response["msgid"] = ADD_FRIEND_MSG_ACK;

    // 不能加自己
    if (userid == friendid)
    {
        response["errno"] = 1;
        response["errmsg"] = "cannot add yourself";
        sendTcpJson(conn, response);
        return;
    }

    // 先检查好友是否存在
    User friendUser = _userModel.query(friendid);
    if (friendUser.getId() != friendid)
    {
        response["errno"] = 1;
        response["errmsg"] = "friend user does not exist";
        sendTcpJson(conn, response);
        return;
    }

    // 插入双向好友关系
    if (!_friendModel.insert(userid, friendid) || !_friendModel.insert(friendid, userid))
    {
        response["errno"] = 1;
        response["errmsg"] = "add friend failed";
        sendTcpJson(conn, response);
        return;
    }

    response["errno"] = 0;
    response["friendid"] = friendUser.getId();
    response["friendname"] = friendUser.getName();
    response["state"] = friendUser.getState();

    sendTcpJson(conn, response);

    // 通知被添加方（如果在线上，让对方好友列表同步刷新）
    {
        User selfUser = _userModel.query(userid);
        json notify;
        notify["msgid"] = ADD_FRIEND_MSG_ACK;
        notify["errno"] = 0;
        notify["friendid"] = selfUser.getId();
        notify["friendname"] = selfUser.getName();
        notify["state"] = selfUser.getState();

        // 本地在线 → 直接推送
        if (auto friendSession = _sessionManager.sessionForUser(friendid);
            friendSession && _presence.ownsUser(friendid, friendSession->id()))
        {
            sendTcpJson(friendSession->connection(), notify);
            return;
        }
        routeToUser(friendid, userid, ADD_FRIEND_MSG_ACK, notify);
    }
}

// 创建群组业务
void ChatService::createGroup(const TcpConnectionPtr &conn, json &js, Timestamp time)
{
    int userid = js["id"].get<int>();
    string name = js["groupname"];
    string desc = js["groupdesc"];

    json response;
    response["msgid"] = CREATE_GROUP_MSG_ACK;

    if (name.empty())
    {
        response["errno"] = 1;
        response["errmsg"] = "group name cannot be empty";
        sendTcpJson(conn, response);
        return;
    }

    // 群名是否已存在
    Group existed = _groupModel.queryGroupByName(name);
    if (existed.getId() != -1)
    {
        response["errno"] = 1;
        response["errmsg"] = "group name already exists";
        sendTcpJson(conn, response);
        return;
    }

    Group group(-1, name, desc);

    if (!_groupModel.createGroup(group))
    {
        response["errno"] = 1;
        response["errmsg"] = "create group failed";
        sendTcpJson(conn, response);
        return;
    }

    // 创建者加入群
    if (!_groupModel.addGroup(userid, group.getId(), "creator"))
    {
        response["errno"] = 1;
        response["errmsg"] = "create group owner relation failed";
        sendTcpJson(conn, response);
        return;
    }

    response["errno"] = 0;
    response["groupid"] = group.getId();
    response["groupname"] = group.getName();
    response["groupdesc"] = group.getDesc();

    sendTcpJson(conn, response);
}

// 加入群组业务
void ChatService::addGroup(const TcpConnectionPtr &conn, json &js, Timestamp time)
{
    int userid = js["id"].get<int>();
    int groupid = js["groupid"].get<int>();

    json response;
    response["msgid"] = ADD_GROUP_MSG_ACK;

    // 先检查群是否存在
    Group group = _groupModel.queryGroup(groupid);
    if (group.getId() == -1)
    {
        response["errno"] = 1;
        response["errmsg"] = "group does not exist";
        sendTcpJson(conn, response);
        return;
    }

    // 检查是否已在群中
    if (_groupModel.isUserInGroup(userid, groupid))
    {
        response["errno"] = 1;
        response["errmsg"] = "already in group";
        sendTcpJson(conn, response);
        return;
    }

    if (!_groupModel.addGroup(userid, groupid, "normal"))
    {
        response["errno"] = 1;
        response["errmsg"] = "join group failed";
        sendTcpJson(conn, response);
        return;
    }

    response["errno"] = 0;
    response["groupid"] = group.getId();
    response["groupname"] = group.getName();
    response["groupdesc"] = group.getDesc();

    sendTcpJson(conn, response);
}

// 群组聊天业务
void ChatService::groupChat(const TcpConnectionPtr &conn, json &js, Timestamp time)
{
    int userid = js["id"].get<int>();
    int groupid = js["groupid"].get<int>();
    string msg = js["msg"];

    auto sender = _sessionManager.sessionForConnection(conn);
    if (!sender || sender->userId() != userid ||
        !_presence.ownsUser(userid, sender->id()))
    {
        ++_routeStale;
        sendTcpJson(conn, json{{"msgid", GROUP_CHAT_MSG_ACK}, {"errno", 6},
                               {"errmsg", "sender is not current owner"}});
        return;
    }

    // 限流检查
    if (!RateLimiter::instance()->allow(userid))
    {
        json response;
        response["msgid"] = GROUP_CHAT_MSG_ACK;
        response["errno"] = 3;
        response["errmsg"] = "rate limit exceeded, slow down";
        sendTcpJson(conn, response);
        return;
    }

    // 异步落库群聊历史消息（快照发送者昵称）
    {
        string gname = js.value("name", "");
        string gextra = gname.empty() ? "" : json{{"name", gname}}.dump();
        if (DBThreadPool::instance()->submitVoid([this, userid, groupid, msg, gextra]() {
            _chatHistoryModel.insertGroupChat(userid, groupid, msg, 0, gextra);
        }) != DBThreadPool::SubmitResult::Accepted) {
            const auto count = ++_historyRejected;
            if (count == 1 || count % 100 == 0)
                LOG_WARN << "GROUP_HISTORY_QUEUE_REJECTED count=" << count;
        }
    }

    vector<int> useridVec = _groupModel.queryGroupUsers(userid, groupid);

    for (int id : useridVec)
    {
        if (auto session = _sessionManager.sessionForUser(id);
            session && _presence.ownsUser(id, session->id()))
        {
            if (auto target = session->connection()) sendTcpJson(target, js);
            continue;
        }
        const auto status = routeToUser(id, userid, GROUP_CHAT_MSG, js);
        if (status == RouteStatus::Offline) storeLegacyGroupOffline(userid, id, js);
    }
}

ChatService::RouteStatus ChatService::routeToUser(
    int receiverId, int senderId, int messageType, const json& payload)
{
    auto found = _presence.lookupUser(receiverId);
    if (found.status == cluster::LookupStatus::Error)
    {
        ++_routeFailed;
        LOG_ERROR << "ROUTE_FAILED presence lookup user=" << receiverId;
        return RouteStatus::Error;
    }
    if (found.status == cluster::LookupStatus::Miss) return RouteStatus::Offline;
    if (found.owner.nodeId == _presence.nodeId())
    {
        // Local Session vanished after a prior snapshot. This owner is not deliverable.
        if (_sessionManager.sessionForUser(receiverId)) return RouteStatus::Error;
        _presence.unbindUser(receiverId, found.owner.sessionId);
        return RouteStatus::Offline;
    }
    cluster::RoutedMessage route;
    route.routeId = _route.nextRouteId();
    route.sourceNode = _presence.nodeId();
    route.targetNode = found.owner.nodeId;
    route.messageType = messageType;
    route.senderUserId = senderId;
    route.receiverUserId = receiverId;
    route.payload = payload;
    if (!_route.send(route))
    {
        ++_routeFailed;
        LOG_ERROR << "ROUTE_FAILED xadd route=" << route.routeId
                  << " target=" << route.targetNode;
        return RouteStatus::Error;
    }
    ++_routeRemote;
    LOG_DEBUG << "ROUTE_REMOTE id=" << route.routeId << " target=" << route.targetNode;
    return RouteStatus::Sent;
}

void ChatService::storeLegacyGroupOffline(int senderId, int receiverId, const json& payload)
{
    const auto encoded = payload.dump();
    if (DBThreadPool::instance()->submitVoid([this, senderId, receiverId, encoded]() {
        _chatHistoryModel.insertOneChat(senderId, receiverId, encoded, 2);
    }) != DBThreadPool::SubmitResult::Accepted) {
        const auto count = ++_historyRejected;
        if (count == 1 || count % 100 == 0)
            LOG_ERROR << "GROUP_OFFLINE_QUEUE_REJECTED count=" << count;
    }
}

void ChatService::storeDurableOffline(
    int senderId, int receiverId, const json& payload,
    const TcpConnectionPtr& sender, std::function<void()> accepted)
{
    (void)senderId;
    const auto encoded = payload.dump();
    std::weak_ptr<TcpConnection> weakSender = sender;
    auto* senderLoop = sender->getLoop();
    auto complete = [this, weakSender, senderLoop,
                     receiverId, accepted = std::move(accepted)](
                        gateway::PersistResult saved) mutable {
        senderLoop->queueInLoop([this, weakSender, receiverId,
                                 saved = std::move(saved), accepted]() mutable {
            if (saved.status == gateway::PersistStatus::Error) {
                ++_durableDbFailed;
                if (auto current = weakSender.lock())
                    sendTcpJson(current, json{{"msgid", ONE_CHAT_MSG_ACK},
                        {"errno", 4}, {"errmsg", "durable offline inbox unavailable"}});
                return;
            }
            ++_waitingCreated;
            LOG_INFO << "OFFLINE_DURABLE_ACCEPT id=" << saved.record.deliveryId
                     << " user=" << receiverId;
            accepted();
            // Covers a receiver login racing with the DB INSERT on this node.
            if (auto session = _sessionManager.sessionForUser(receiverId);
                session && _presence.ownsUser(receiverId, session->id())) {
                enqueueInbox(receiverId);
            }
        });
    };
    if (!DBThreadPool::instance()->trySubmitDurable(
        [this, receiverId, encoded, complete = std::move(complete)]() mutable {
            gateway::PersistResult saved;
            if (!std::getenv("CHAT_PHASE7_TEST_REJECT_OFFLINE_PERSIST"))
                saved = _durablePendingModel.persistOffline(
                    receiverId, ONE_CHAT_MSG, encoded, gateway::unixMillis());
            complete(std::move(saved));
        })) {
        ++_durableDbFailed;
        if (auto current = weakSender.lock())
            sendTcpJson(current, json{{"msgid", ONE_CHAT_MSG_ACK},
                {"errno", 4}, {"errmsg", "durable offline inbox queue unavailable"}});
    }
}

cluster::RouteOutcome ChatService::handleRoutedMessage(cluster::RoutedMessage message)
{
    ++_routeReceived;
    if (message.targetNode != _presence.nodeId() ||
        !message.payload.contains("msgid") ||
        !message.payload["msgid"].is_number_integer() ||
        message.payload["msgid"].get<int>() != message.messageType ||
        (message.messageType == ONE_CHAT_MSG &&
         (!message.payload.contains("toid") ||
          !message.payload["toid"].is_number_integer() ||
          message.payload["toid"].get<int>() != message.receiverUserId)))
    {
        ++_routeFailed;
        LOG_WARN << "ROUTE_FAILED malformed target=" << message.targetNode;
        return cluster::RouteOutcome::Terminal;
    }
    // Stream consumer thread only locates the loop. It never touches client context.
    auto session = _sessionManager.sessionForUser(message.receiverUserId);
    auto connection = session ? session->connection() : nullptr;
    auto* loop = connection ? connection->getLoop() : _mainLoop.load();
    if (!loop) { ++_routeFailed; return cluster::RouteOutcome::Retry; }
    auto result = std::make_shared<std::promise<cluster::RouteOutcome>>();
    auto future = result->get_future();
    auto finished = std::make_shared<std::atomic<bool>>(false);
    loop->queueInLoop([this, message = std::move(message), result, finished]() mutable {
        if (finished->load()) return;
        auto complete = [result, finished](cluster::RouteOutcome outcome) {
            if (!finished->exchange(true)) result->set_value(outcome);
        };
        try { processRoutedMessage(std::move(message), complete); }
        catch (...) { complete(cluster::RouteOutcome::Retry); }
    });
    if (future.wait_for(std::chrono::seconds(3)) == std::future_status::ready)
        return future.get();
    finished->store(true);
    ++_routeFailed;
    LOG_ERROR << "ROUTE_ACCEPT_TIMEOUT";
    return cluster::RouteOutcome::RetryInfrastructure;
}

void ChatService::processRoutedMessage(
    cluster::RoutedMessage message,
    std::function<void(cluster::RouteOutcome)> complete)
{
    auto owner = _presence.lookupUser(message.receiverUserId);
    auto session = _sessionManager.sessionForUser(message.receiverUserId);
    if (owner.status == cluster::LookupStatus::Hit &&
        owner.owner.nodeId == _presence.nodeId() && session &&
        owner.owner.sessionId == session->id() &&
        _presence.ownsUser(message.receiverUserId, session->id()))
    {
        if (message.messageType == ONE_CHAT_MSG) {
            auto target = session->connection();
            if (!target) { complete(cluster::RouteOutcome::Retry); return; }
            const auto* state = boost::any_cast<protocol::ProtocolState>(&target->getContext());
            if (state && state->selected() == protocol::WireProtocol::Binary) {
                const auto marked = _route.markAccepted(message.routeId);
                if (marked == cluster::RouteTransport::DedupResult::Error)
                    complete(cluster::RouteOutcome::Retry);
                else {
                    if (marked == cluster::RouteTransport::DedupResult::New)
                        ChatServer::sendResponse(target, message.payload);
                    complete(cluster::RouteOutcome::Accepted);
                }
                return;
            }
            persistAndDeliver(session, message.payload, message.routeId,
                [complete = std::move(complete)](bool accepted) {
                    complete(accepted ? cluster::RouteOutcome::Accepted
                                      : cluster::RouteOutcome::RetryInfrastructure);
                });
            return;
        }
        if (auto target = session->connection()) {
            const auto marked = _route.markAccepted(message.routeId);
            if (marked == cluster::RouteTransport::DedupResult::Error) {
                complete(cluster::RouteOutcome::Retry);
                return;
            }
            if (marked == cluster::RouteTransport::DedupResult::New)
                ChatServer::sendResponse(target, message.payload);
            complete(cluster::RouteOutcome::Accepted);
            return;
        }
        complete(cluster::RouteOutcome::Retry);
        return;
    }
    if (owner.status == cluster::LookupStatus::Miss &&
        message.messageType == ONE_CHAT_MSG) {
        // The target disconnected after the source resolved Presence. Keep
        // the routeId as the durable idempotency key before Stream XACK.
        auto* loop = muduo::net::EventLoop::getEventLoopOfCurrentThread();
        const auto routeId = message.routeId;
        const auto user = message.receiverUserId;
        const auto encoded = message.payload.dump();
        auto completion = std::make_shared<std::function<void(cluster::RouteOutcome)>>(std::move(complete));
        if (!DBThreadPool::instance()->trySubmitDurable(
            [this, loop, routeId, user, encoded, completion]() mutable {
                gateway::PersistResult saved;
                if (!std::getenv("CHAT_PHASE7_TEST_REJECT_OFFLINE_PERSIST"))
                    saved = _durablePendingModel.persistOffline(
                        user, ONE_CHAT_MSG, encoded, gateway::unixMillis(), routeId);
                loop->queueInLoop([this, routeId, user, saved = std::move(saved),
                                   completion]() mutable {
                    if (saved.status == gateway::PersistStatus::Error) {
                        ++_durableDbFailed;
                        (*completion)(cluster::RouteOutcome::RetryInfrastructure);
                        return;
                    }
                    if (_route.markAccepted(routeId) ==
                        cluster::RouteTransport::DedupResult::Error) {
                        ++_routeFailed;
                        (*completion)(cluster::RouteOutcome::RetryInfrastructure);
                        return;
                    }
                    if (saved.status == gateway::PersistStatus::Inserted)
                        ++_waitingCreated;
                    if (auto session = _sessionManager.sessionForUser(user);
                        session && _presence.ownsUser(user, session->id())) {
                        enqueueInbox(user);
                    }
                    (*completion)(cluster::RouteOutcome::Accepted);
                });
            })) {
            ++_durableDbFailed;
            (*completion)(cluster::RouteOutcome::RetryInfrastructure);
        }
        return;
    }
    ++_routeStale;
    LOG_WARN << "ROUTE_STALE id=" << message.routeId
             << " user=" << message.receiverUserId;
    if (owner.status == cluster::LookupStatus::Hit &&
        owner.owner.nodeId != _presence.nodeId() && message.hops == 0)
    {
        message.targetNode = owner.owner.nodeId;
        message.hops = 1;
        if (_route.send(message)) {
            ++_routeRemote;
            complete(cluster::RouteOutcome::Accepted);
            return;
        }
    }
    ++_routeFailed;
    // Keep the PEL entry for bounded reclaims. Dead-letter after max attempts.
    complete(cluster::RouteOutcome::Retry);
}

bool ChatService::activateDurable(
    const std::shared_ptr<const gateway::Session>& session,
    const gateway::DurablePendingRecord& record)
{
    auto target = session ? session->connection() : nullptr;
    if (!target || !target->connected()) return false;
    if (target->getLoop() != muduo::net::EventLoop::getEventLoopOfCurrentThread())
        return false;
    if (ChatServer::isBackpressured(target)) return false;
    if (_sessionManager.sessionForUser(session->userId()) != session ||
        !_presence.ownsUser(session->userId(), session->id())) return false;
    try {
        auto payload = json::parse(record.payload);
        gateway::MessageEnvelope delivery(
            record.messageType, std::move(payload),
            protocol::WireProtocol::Undetermined, record.deliveryId);
        const auto restored = _pendingDeliveries.restore(
            session, delivery, record.retryCount);
        if (restored == gateway::RestoreResult::CapacityExceeded) return false;
        if (restored == gateway::RestoreResult::Inserted) {
            LOG_DEBUG << "DURABLE_DELIVERY_SEND id=" << record.deliveryId
                     << " user=" << record.receiverUserId;
            ChatServer::sendResponse(target, delivery);
        }
        return true;
    } catch (...) {
        LOG_ERROR << "DURABLE_DELIVERY invalid payload id=" << record.deliveryId;
        return false;
    }
}

void ChatService::persistAndDeliver(
    const std::shared_ptr<const gateway::Session>& session,
    const json& payload, const std::string& routeId,
    std::function<void(bool)> complete)
{
    auto target = session ? session->connection() : nullptr;
    if (!target || !complete) { if (complete) complete(false); return; }
    auto* loop = target->getLoop();
    std::weak_ptr<const gateway::Session> weakSession = session;
    const auto node = _presence.nodeId();
    const auto user = session->userId();
    const auto encoded = payload.dump();
    auto completion = std::make_shared<std::function<void(bool)>>(std::move(complete));
    const bool queued = DBThreadPool::instance()->trySubmitDurable(
        [this, loop, weakSession, routeId, node, user, encoded,
         completion]() mutable {
            gateway::PersistResult saved;
            try {
                // Fault hooks are opt-in and used only by the integration
                // harness; normal deployments never set these variables.
                if (std::getenv("CHAT_PHASE6_TEST_REJECT_PERSIST")) {
                    LOG_WARN << "DURABLE_TEST_REJECT_PERSIST route=" << routeId;
                } else {
                    saved = _durablePendingModel.persist(
                        routeId, node, user, ONE_CHAT_MSG, encoded,
                        gateway::unixMillis());
                }
            } catch (const std::exception& error) {
                LOG_ERROR << "DURABLE_PERSIST exception=" << error.what();
            } catch (...) {
                LOG_ERROR << "DURABLE_PERSIST unknown exception";
            }
            if (saved.status != gateway::PersistStatus::Error &&
                std::getenv("CHAT_PHASE6_TEST_STOP_AFTER_PERSIST")) {
                LOG_WARN << "DURABLE_TEST_STOP_AFTER_PERSIST id="
                         << saved.record.deliveryId;
                std::raise(SIGSTOP);
            }
            loop->queueInLoop([this, weakSession, routeId,
                               saved = std::move(saved),
                               completion]() mutable {
                if (saved.status == gateway::PersistStatus::Error) {
                    ++_durableDbFailed;
                    (*completion)(false);
                    return;
                }
                if (saved.status == gateway::PersistStatus::Inserted)
                    ++_durablePersisted;
                if (!routeId.empty() &&
                    _route.markAccepted(routeId) ==
                        cluster::RouteTransport::DedupResult::Error) {
                    ++_routeFailed;
                    (*completion)(false);
                    return;
                }
                // A duplicate may arrive after the client ACK. Still update
                // Phase 5 dedup metrics before accepting its Stream entry.
                if (saved.record.state == gateway::DurableState::Acked ||
                    saved.record.state == gateway::DurableState::Expired) {
                    (*completion)(true);
                    return;
                }
                // Existing row may have been sent before the crash. The durable
                // scanner, not this duplicate route, controls its next attempt.
                if (saved.status == gateway::PersistStatus::Inserted) {
                    if (auto current = weakSession.lock())
                        activateDurable(current, saved.record);
                }
                (*completion)(true); // Durable source exists even if Session changed.
            });
        });
    if (!queued) {
        ++_durableDbFailed;
        (*completion)(false);
    }
}

void ChatService::recoverAtStartup()
{
    const auto node = _presence.nodeId();
    if (!DBThreadPool::instance()->trySubmitDurable([this, node]() {
            auto count = _durablePendingModel.recoverStartup(node, gateway::unixMillis());
            if (count) {
                _durableRecovered += *count;
                LOG_INFO << "DURABLE_STARTUP_RECOVERY node=" << node
                         << " pending=" << *count;
            } else {
                ++_durableDbFailed;
                LOG_ERROR << "DURABLE_STARTUP_RECOVERY failed node=" << node;
            }
        }))
        ++_durableDbFailed;
}

void ChatService::recoverForSession(
    const std::shared_ptr<const gateway::Session>& session)
{
    auto target = session ? session->connection() : nullptr;
    if (!target) return;
    if (ChatServer::isBackpressured(target)) {
        std::lock_guard<std::mutex> lock(_inboxMutex);
        _inboxInFlight.erase(session->userId());
        return;
    }
    auto* loop = target->getLoop();
    const auto user = session->userId();
    const auto node = _presence.nodeId();
    const auto queue = DBThreadPool::instance()->metrics();
    const auto reserved = queue.capacity / 4;
    const auto headroom = queue.depth < queue.capacity - reserved
        ? queue.capacity - reserved - queue.depth : 0;
    const auto limit = std::min({static_cast<std::size_t>(Config::instance().getOfflineClaimBatch()),
                                 _pendingDeliveries.availableSlots(), headroom});
    const auto maxBytes = _pendingDeliveries.availableBytes();
    if (!limit || !maxBytes) {
        std::lock_guard<std::mutex> lock(_inboxMutex);
        _inboxInFlight.erase(user);
        return;
    }
    std::weak_ptr<const gateway::Session> weak = session;
    if (!DBThreadPool::instance()->trySubmitDurable(
        [this, loop, weak, user, node, limit, maxBytes]() {
            auto migrated = _durablePendingModel.migrateLegacyForUser(
                user, gateway::unixMillis(), 100);
            std::optional<std::vector<gateway::DurablePendingRecord>> rows;
            if (!std::getenv("CHAT_PHASE7_TEST_REJECT_CLAIM"))
                rows = _durablePendingModel.claimForUser(
                    user, node, gateway::unixMillis(), limit, maxBytes);
            loop->queueInLoop([this, weak, user, limit, migrated,
                               rows = std::move(rows)]() mutable {
                {
                    std::lock_guard<std::mutex> lock(_inboxMutex);
                    _inboxInFlight.erase(user);
                    if (rows && rows->size() < limit && migrated && *migrated < 100)
                        _inboxQueued.erase(user);
                }
                if (migrated) _legacyMigrated += *migrated;
                else { ++_legacyFailed; ++_durableDbFailed; }
                if (!rows) {
                    ++_durableDbFailed;
                    LOG_ERROR << "DURABLE_SESSION_RECOVERY failed";
                    return;
                }
                auto current = weak.lock();
                if (!current || _sessionManager.sessionForUser(current->userId()) != current ||
                    !_presence.ownsUser(current->userId(), current->id())) {
                    // A slower old-node login claim must not strand the row
                    // after a newer login has become the Redis owner.
                    if (current && !rows->empty()) {
                        auto owner = _presence.lookupUser(current->userId());
                        if (owner.status == cluster::LookupStatus::Hit &&
                            owner.owner.nodeId != _presence.nodeId()) {
                            const auto oldNode = _presence.nodeId();
                            const auto newNode = owner.owner.nodeId;
                            auto claimed = std::move(*rows);
                            if (!DBThreadPool::instance()->trySubmitDurable(
                                [this, claimed = std::move(claimed), oldNode, newNode]() {
                                    for (const auto& row : claimed)
                                        if (!_durablePendingModel.transferOwner(
                                                row.deliveryId, oldNode, newNode,
                                                gateway::unixMillis()))
                                            ++_durableDbFailed;
                                })) ++_durableDbFailed;
                        }
                    }
                    return;
                }
                _waitingClaimed += rows->size();
                _durableRecovered += rows->size();
                // The target I/O loop emits this batch in delivery_id order.
                // Persisted next_retry_at prevents a concurrent scan from
                // racing these first sends.
                for (const auto& row : *rows)
                    if (activateDurable(current, row)) ++_offlineRecovered;
            });
        }, DBThreadPool::Priority::Low)) {
        std::lock_guard<std::mutex> lock(_inboxMutex);
        _inboxInFlight.erase(user);
        ++_durableDbFailed;
    }
}

void ChatService::scanInboxRecovery()
{
    std::vector<int> users;
    {
        std::lock_guard<std::mutex> lock(_inboxMutex);
        const auto candidates = _inboxOrder.size();
        for (std::size_t i = 0; i < candidates &&
             users.size() < static_cast<std::size_t>(Config::instance().getOfflineUsersPerRound()); ++i) {
            const int user = _inboxOrder.front();
            _inboxOrder.pop_front();
            if (!_inboxQueued.count(user)) continue;
            _inboxOrder.push_back(user); // fair round-robin across users
            if (_inboxInFlight.insert(user).second) users.push_back(user);
        }
    }
    for (int user : users) {
        auto session = _sessionManager.sessionForUser(user);
        if (!session || !_presence.ownsUser(user, session->id())) {
            std::lock_guard<std::mutex> lock(_inboxMutex);
            _inboxQueued.erase(user);
            _inboxInFlight.erase(user);
            continue;
        }
        auto target = session->connection();
        if (!target) {
            std::lock_guard<std::mutex> lock(_inboxMutex);
            _inboxInFlight.erase(user);
            continue;
        }
        std::weak_ptr<const gateway::Session> weak = session;
        target->getLoop()->runInLoop([this, weak, user]() {
            if (auto current = weak.lock()) recoverForSession(current);
            else {
                std::lock_guard<std::mutex> lock(_inboxMutex);
                _inboxInFlight.erase(user);
            }
        });
    }
}

void ChatService::enqueueInbox(int userId)
{
    if (userId <= 0) return;
    std::lock_guard<std::mutex> lock(_inboxMutex);
    if (_inboxQueued.insert(userId).second)
        _inboxOrder.push_back(userId);
}

// 查询单聊历史消息
void ChatService::queryOneChatHistory(const TcpConnectionPtr &conn, json &js, Timestamp time)
{
    int userid = js["id"].get<int>();
    int limit = js["limit"].get<int>();

    vector<string> vec = _chatHistoryModel.queryRecentOneChat(userid, limit);

    // 额外做一份 protobuf 映射
    auto pbResponse = ProtoUtils::buildOneChatHistoryResponse(userid, vec);

    // 当前阶段：只做内部验证，不真正切换客户端协议
    if (Config::instance().useProtobufHistory())
    {
        string pbData;
        pbResponse.SerializeToString(&pbData);
        cout << "[protobuf history] one chat response bytes = " << pbData.size() << endl;
    }

    json response;
    response["msgid"] = QUERY_ONE_CHAT_HISTORY_MSG_ACK;
    response["errno"] = 0;
    response["id"] = userid;
    response["history"] = vec;

    sendTcpJson(conn, response);
}

// 查询群聊历史消息
void ChatService::queryGroupChatHistory(const TcpConnectionPtr &conn, json &js, Timestamp time)
{
    int groupid = js["groupid"].get<int>();
    int limit = js["limit"].get<int>();

    vector<string> vec = _chatHistoryModel.queryRecentGroupChat(groupid, limit);

    // 额外做一份 protobuf 映射
    auto pbResponse = ProtoUtils::buildGroupChatHistoryResponse(groupid, vec);

    // 当前阶段：只做内部验证，不真正切换客户端协议
    if (Config::instance().useProtobufHistory())
    {
        string pbData;
        pbResponse.SerializeToString(&pbData);
        cout << "[protobuf history] group chat response bytes = " << pbData.size() << endl;
    }

    json response;
    response["msgid"] = QUERY_GROUP_CHAT_HISTORY_MSG_ACK;
    response["errno"] = 0;
    response["groupid"] = groupid;
    response["history"] = vec;

    sendTcpJson(conn, response);
}

void ChatService::messageAck(const TcpConnectionPtr &conn, json &js, Timestamp)
{
    const auto deliveryId = gateway::parseDeliveryId(js);
    if (!deliveryId) return; // Decoder 已拒绝非法 wire；此处作防御性检查。
    const auto result = _pendingDeliveries.validateAck(*deliveryId, conn);
    if (result == gateway::AckResult::Acked)
    {
        auto session = _sessionManager.sessionForConnection(conn);
        if (!session) return;
        {
            std::lock_guard<std::mutex> lock(_ackMutex);
            if (_deferredAcks.size() >= 10000 && !_deferredAcks.count(*deliveryId)) {
                ++_durableDbFailed;
                LOG_ERROR << "DURABLE_ACK deferred capacity exhausted id=" << *deliveryId;
                return;
            }
            _deferredAcks.emplace(*deliveryId, session->userId());
        }
        submitDeferredAck(*deliveryId, session->userId());
    }
    else if (result == gateway::AckResult::Unknown)
    {
        LOG_DEBUG << "DELIVERY_DUPLICATE or unknown ACK id=" << *deliveryId;
    }
    else
    {
        LOG_WARN << "DELIVERY_ACK wrong session id=" << *deliveryId;
    }
}

void ChatService::submitDeferredAck(std::uint64_t id, int userId)
{
    {
        std::lock_guard<std::mutex> lock(_ackMutex);
        if (!_deferredAcks.count(id) || !_ackInFlight.insert(id).second) return;
    }
    if (!DBThreadPool::instance()->trySubmitDurable([this, id, userId]() {
        const auto persisted = _durablePendingModel.markAcked(
            id, userId, gateway::unixMillis());
        {
            std::lock_guard<std::mutex> lock(_ackMutex);
            _ackInFlight.erase(id);
            if (persisted != gateway::DurableAckStatus::Error)
                _deferredAcks.erase(id);
        }
        if (persisted == gateway::DurableAckStatus::Error) {
            ++_durableDbFailed;
            const auto failures = ++_ackDbFailures;
            if (failures == 1 || failures % 100 == 0)
                LOG_WARN << "DURABLE_ACK deferred failures=" << failures;
        } else if (persisted == gateway::DurableAckStatus::Terminal) {
            LOG_WARN << "DURABLE_ACK terminal id=" << id;
            _pendingDeliveries.cancel(id);
        } else if (_pendingDeliveries.completeDurableAck(id)) {
            ++_durableAcked;
            LOG_DEBUG << "DELIVERY_ACK id=" << id;
        }
    }, DBThreadPool::Priority::High)) {
        std::lock_guard<std::mutex> lock(_ackMutex);
        _ackInFlight.erase(id);
        ++_ackDeferredTotal;
    }
}

void ChatService::scanDeferredAcks()
{
    std::vector<std::pair<std::uint64_t, int>> batch;
    {
        std::lock_guard<std::mutex> lock(_ackMutex);
        for (const auto& entry : _deferredAcks) {
            if (!_ackInFlight.count(entry.first)) batch.push_back(entry);
            if (batch.size() == 128) break;
        }
    }
    for (const auto& [id, user] : batch) submitDeferredAck(id, user);
}

bool ChatService::hasDeferredAck(std::uint64_t id) const
{
    std::lock_guard<std::mutex> lock(_ackMutex);
    return _deferredAcks.count(id) != 0;
}

void ChatService::scanReliableDeliveries()
{
    scanDeferredAcks();
    scanInboxRecovery();
    if (_durableScanInFlight.exchange(true)) return;
    auto* mainLoop = _mainLoop.load();
    if (!mainLoop) { _durableScanInFlight = false; return; }
    const auto node = _presence.nodeId();
    static std::uint64_t scanCount = 0; // called only on the main EventLoop
    const bool housekeeping = (++scanCount % 60 == 0);
    const bool discoverInbox = scanCount % 10 == 0;
    if (!DBThreadPool::instance()->trySubmitDurable([this, mainLoop, node, housekeeping, discoverInbox]() {
            const auto now = gateway::unixMillis();
            if (housekeeping) {
                auto expired = _durablePendingModel.expireOverdue(now);
                if (!expired) ++_durableDbFailed;
                else _durableExpired += *expired;
                if (!_durablePendingModel.cleanupTerminal(
                        now - 7ULL * 24 * 60 * 60 * 1000))
                    ++_durableDbFailed;
                auto counts = _durablePendingModel.activeCounts();
                if (counts) {
                    _dbPendingCount = counts->first;
                    _dbWaitingCount = counts->second;
                } else ++_durableDbFailed;
            }
            std::optional<std::vector<int>> waitingUsers;
            if (discoverInbox) {
                waitingUsers = _durablePendingModel.waitingUsersAfter(_waitingCursor);
                if (waitingUsers && waitingUsers->empty() && _waitingCursor > 0) {
                    _waitingCursor = 0;
                    waitingUsers = _durablePendingModel.waitingUsersAfter(0);
                }
                if (waitingUsers && !waitingUsers->empty())
                    _waitingCursor = waitingUsers->back();
            }
            auto due = _durablePendingModel.dueForNode(node, now);
            mainLoop->queueInLoop([this, due = std::move(due), discoverInbox,
                                   waitingUsers = std::move(waitingUsers)]() mutable {
                _durableScanInFlight = false;
                if (waitingUsers) {
                    for (int user : *waitingUsers) {
                        auto session = _sessionManager.sessionForUser(user);
                        if (session && _presence.ownsUser(user, session->id())) {
                            enqueueInbox(user);
                        }
                    }
                } else if (discoverInbox) ++_durableDbFailed;
                if (!due) { ++_durableDbFailed; return; }
                for (auto& row : *due) handleDueDurableRecord(std::move(row));
            });
        }, DBThreadPool::Priority::Low)) {
        _durableScanInFlight = false;
        ++_durableDbFailed;
    }
}

void ChatService::handleDueDurableRecord(gateway::DurablePendingRecord record)
{
    auto session = _sessionManager.sessionForUser(record.receiverUserId);
    auto connection = session ? session->connection() : nullptr;
    auto* loop = connection ? connection->getLoop() : _mainLoop.load();
    if (!loop) return;
    std::weak_ptr<const gateway::Session> weak = session;
    loop->queueInLoop([this, weak, record = std::move(record), loop]() mutable {
        if (hasDeferredAck(record.deliveryId)) return;
        auto current = weak.lock();
        auto target = current ? current->connection() : nullptr;
        const auto now = gateway::unixMillis();
        const auto node = _presence.nodeId();
        if (record.expireAtMs <= now) {
            if (!DBThreadPool::instance()->trySubmitDurable(
                [this, loop, id = record.deliveryId, now]() {
                    const auto changed = _durablePendingModel.markExpired(id, now);
                    loop->queueInLoop([this, id, changed]() {
                        if (changed && *changed) { ++_durableExpired; _pendingDeliveries.cancel(id); }
                        else if (!changed) ++_durableDbFailed;
                    });
                })) ++_durableDbFailed;
            return;
        }
        const auto* state = target
            ? boost::any_cast<protocol::ProtocolState>(&target->getContext()) : nullptr;
        if (!target || !target->connected() ||
            _sessionManager.sessionForUser(record.receiverUserId) != current ||
            !_presence.ownsUser(record.receiverUserId, current->id()) ||
            (state && state->selected() == protocol::WireProtocol::Binary)) {
            _pendingDeliveries.suspend(record.deliveryId);
            auto owner = _presence.lookupUser(record.receiverUserId);
            if (owner.status == cluster::LookupStatus::Hit &&
                owner.owner.nodeId != node) {
                if (!DBThreadPool::instance()->trySubmitDurable(
                    [this, id = record.deliveryId, node,
                     newNode = owner.owner.nodeId]() {
                        if (!_durablePendingModel.transferOwner(
                                id, node, newNode, gateway::unixMillis()))
                            ++_durableDbFailed;
                    })) ++_durableDbFailed;
                return;
            }
            if (!DBThreadPool::instance()->trySubmitDurable(
                [this, id = record.deliveryId, node,
                 expected = record.nextRetryAtMs]() {
                    auto changed = _durablePendingModel.markWaiting(
                        id, node, expected, gateway::unixMillis());
                    if (!changed) ++_durableDbFailed;
                    else if (*changed) ++_waitingRequeued;
                })) ++_durableDbFailed;
            return;
        }
        if (ChatServer::isBackpressured(target)) return;
        if (record.retryCount >= 2) {
            if (!DBThreadPool::instance()->trySubmitDurable(
                [this, loop, id = record.deliveryId, now]() {
                    const auto changed = _durablePendingModel.markExpired(id, now);
                    loop->queueInLoop([this, id, changed]() {
                        if (changed && *changed) { ++_durableExpired; _pendingDeliveries.cancel(id); }
                        else if (!changed) ++_durableDbFailed;
                    });
                })) ++_durableDbFailed;
            return;
        }
        json decoded;
        try { decoded = json::parse(record.payload); }
        catch (...) {
            LOG_ERROR << "DURABLE_DELIVERY invalid payload id=" << record.deliveryId;
            if (!DBThreadPool::instance()->trySubmitDurable(
                [this, id = record.deliveryId]() {
                    auto changed = _durablePendingModel.markExpired(id, gateway::unixMillis());
                    if (!changed) ++_durableDbFailed;
                    else if (*changed) ++_durableExpired;
                })) ++_durableDbFailed;
            return;
        }
        gateway::MessageEnvelope delivery(
            record.messageType, std::move(decoded),
            protocol::WireProtocol::Undetermined, record.deliveryId);
        const auto restored = _pendingDeliveries.restore(
            current, delivery, record.retryCount);
        if (restored == gateway::RestoreResult::CapacityExceeded) return;
        std::weak_ptr<const gateway::Session> weakCurrent = current;
        if (!DBThreadPool::instance()->trySubmitDurable(
            [this, loop, weakCurrent, record, delivery]() {
                const auto now = gateway::unixMillis();
                const bool persisted = _durablePendingModel.markRetry(
                    record.deliveryId, _presence.nodeId(), record.retryCount,
                    now + 2000, now);
                loop->queueInLoop([this, weakCurrent, delivery, record, persisted]() {
                    if (!persisted) {
                        ++_durableDbFailed;
                        _pendingDeliveries.suspend(record.deliveryId);
                        return;
                    }
                    auto current = weakCurrent.lock();
                    auto target = current ? current->connection() : nullptr;
                    if (!target || !target->connected() ||
                        _sessionManager.sessionForUser(record.receiverUserId) != current ||
                        !_presence.ownsUser(record.receiverUserId, current->id())) return;
                    if (!_pendingDeliveries.markDurableRetry(
                            record.deliveryId, record.retryCount + 1)) return;
                    LOG_DEBUG << "DURABLE_DELIVERY_RETRY id=" << record.deliveryId
                             << " count=" << record.retryCount + 1;
                    ChatServer::sendResponse(target, delivery);
                });
            })) ++_durableDbFailed;
    });
}

void ChatService::heartbeatPresence()
{
    if (!_presence.heartbeat(_sessionManager.activeSessions()))
        LOG_ERROR << "PRESENCE_HEARTBEAT failed node=" << _presence.nodeId();
}

void ChatService::startHeartbeat()
{
    if (_heartbeatWorker.joinable()) return;
    _heartbeatWorker = std::thread([this]() {
        std::unique_lock<std::mutex> lock(_heartbeatMutex);
        while (!_heartbeatCv.wait_for(lock, std::chrono::seconds(5),
                                      [this] { return _heartbeatStop; })) {
            lock.unlock();
            syncServerConnections();
            heartbeatPresence();
            lock.lock();
        }
    });
}

void ChatService::logClusterMetrics() const
{
    const auto presence = _presence.metrics();
    const auto route = _route.metrics();
    LOG_INFO << "CLUSTER_METRICS node=" << _presence.nodeId()
             << " presence_bind=" << presence.bind
             << " presence_unbind=" << presence.unbind
             << " presence_lookup=" << presence.lookup
             << " presence_hit=" << presence.hit
             << " presence_miss=" << presence.miss
             << " route_local=" << _routeLocal.load()
             << " route_remote=" << _routeRemote.load()
             << " route_received=" << _routeReceived.load()
             << " route_stale=" << _routeStale.load()
             << " route_failed=" << _routeFailed.load()
             << " route_produced=" << route.produced
             << " route_consumed=" << route.consumed
             << " route_acked=" << route.acked
             << " route_retried=" << route.retried
             << " route_claimed=" << route.claimed
             << " route_duplicate=" << route.duplicate
             << " route_transport_failed=" << route.failed
             << " route_pending=" << route.pending
             << " stream_length=" << route.streamLength
             << " stream_peak=" << route.streamPeak
             << " stream_oldest_pending_ms=" << route.oldestPendingMs
             << " stream_lag=" << route.lag
             << " stream_overload_rejected=" << route.overloadRejected
             << " target_stream_peak=" << route.targetStreamPeak
             << " node_heartbeat=" << presence.nodeHeartbeat;
}

void ChatService::logDeliveryMetrics() const
{
    const auto metrics = _pendingDeliveries.metrics();
    const auto db = DBThreadPool::instance()->metrics();
    const auto output = ChatServer::outputMetrics();
    std::size_t deferredAckCount = 0;
    {
        std::lock_guard<std::mutex> lock(_ackMutex);
        deferredAckCount = _deferredAcks.size();
    }
    LOG_INFO << "DELIVERY_METRICS sent=" << metrics.sent
             << " acked=" << metrics.acked
             << " retried=" << metrics.retried
             << " timeout=" << metrics.timedOut
             << " failed=" << metrics.failed
             << " pending=" << metrics.pending
             << " peak=" << metrics.pendingPeak
             << " pending_bytes=" << metrics.pendingBytes
             << " peak_bytes=" << metrics.pendingPeakBytes
             << " durable_persisted=" << _durablePersisted.load()
             << " durable_recovered=" << _durableRecovered.load()
             << " durable_acked=" << _durableAcked.load()
             << " durable_db_failed=" << _durableDbFailed.load()
             << " ack_deferred=" << deferredAckCount
             << " ack_enqueue_rejected=" << _ackDeferredTotal.load()
             << " ack_db_failures=" << _ackDbFailures.load()
             << " history_queue_rejected=" << _historyRejected.load()
             << " durable_waiting_created=" << _waitingCreated.load()
             << " durable_waiting_claimed=" << _waitingClaimed.load()
             << " durable_waiting_requeued=" << _waitingRequeued.load()
             << " durable_offline_recovered=" << _offlineRecovered.load()
             << " durable_expired=" << _durableExpired.load()
             << " legacy_offline_migrated=" << _legacyMigrated.load()
             << " legacy_offline_failed=" << _legacyFailed.load()
             << " db_pending=" << _dbPendingCount.load()
             << " db_waiting=" << _dbWaitingCount.load()
             << " db_queue_depth=" << db.depth
             << " db_queue_capacity=" << db.capacity
             << " db_queue_peak=" << db.peak
             << " db_tasks_rejected=" << db.rejected
             << " db_active_workers=" << db.activeWorkers
             << " db_connection_wait_ms=" << ConnectionPool::instance()->totalWaitMs()
             << " db_connection_wait_count=" << ConnectionPool::instance()->waitCount()
             << " active_connections=" << output.activeConnections
             << " slow_consumers=" << output.slowConsumers
             << " output_buffer_highwater=" << output.highwaterEvents
             << " output_buffer_peak_bytes=" << output.outputPeakBytes
             << " queued_output_peak_bytes=" << output.queuedPeakBytes
             << " slow_consumer_disconnects=" << output.hardDisconnects;
}
