#include "chatservice.hpp"
#include "chatserver.hpp"
#include "public.hpp"
#include "password_utils.hpp"
#include "rate_limiter.hpp"
#include "db_thread_pool.hpp"
#include <muduo/base/Logging.h>
#include <vector>
#include "proto_utils.hpp"
#include "config.hpp"
#include "gateway/delivery_id.hpp"
#include <boost/any.hpp>

using namespace std;
using namespace muduo;

namespace
{
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
    : _presence(_redis, Config::instance().getNodeId()),
      _route(_redis, Config::instance().getNodeId())
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
            handleRoutedMessage(std::move(message));
        })) LOG_ERROR << "ROUTE_SUBSCRIBE failed";
}

// 服务器异常，业务重置方法
void ChatService::reset()
{
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

    const auto cancelled = _pendingDeliveries.cancelConnection(conn);
    if (cancelled)
        LOG_WARN << "DELIVERY_FAILED disconnected session count=" << cancelled;

    syncServerConnections();

    // 旧节点迟到 logout 不得删除新节点 owner，也不得把 MySQL 展示状态写离线。
    if (_presence.unbindUser(userid, session->id()))
        _userModel.updateState(User(userid, "", "", "offline"));
}

// 处理客户端异常退出
void ChatService::clientCloseException(const TcpConnectionPtr &conn)
{
    const auto cancelled = _pendingDeliveries.cancelConnection(conn);
    if (cancelled)
        LOG_WARN << "DELIVERY_FAILED disconnected session count=" << cancelled;
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
        DBThreadPool::instance()->submitVoid([this, fromid, toid, msg, extra]() {
            _chatHistoryModel.insertOneChat(fromid, toid, msg, 0, extra);
        });
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

                    const auto deliveryId = _pendingDeliveries.nextDeliveryId();
                    gateway::MessageEnvelope delivery(
                        ONE_CHAT_MSG, request,
                        protocol::WireProtocol::Undetermined, deliveryId);
                    if (!_pendingDeliveries.track(currentSession, delivery))
                    {
                        if (auto sender = weakSender.lock())
                        {
                            sendTcpJson(sender, json{{"msgid", ONE_CHAT_MSG_ACK},
                                                     {"errno", 4},
                                                     {"errmsg", "reliable delivery capacity exhausted"}});
                        }
                        return;
                    }
                    recordHistory();
                    LOG_DEBUG << "DELIVERY_SEND id=" << deliveryId
                              << " receiver=" << toid;
                    ChatServer::sendResponse(target, delivery);
                });
            return;
        }
    }

    recordHistory();

    const auto status = routeToUser(toid, fromid, ONE_CHAT_MSG, js);
    if (status == RouteStatus::Offline) storeOffline(fromid, toid, js);
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
        DBThreadPool::instance()->submitVoid([this, userid, groupid, msg, gextra]() {
            _chatHistoryModel.insertGroupChat(userid, groupid, msg, 0, gextra);
        });
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
        if (status == RouteStatus::Offline) storeOffline(userid, id, js);
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
        LOG_ERROR << "ROUTE_FAILED publish route=" << route.routeId
                  << " target=" << route.targetNode;
        return RouteStatus::Error;
    }
    ++_routeRemote;
    LOG_INFO << "ROUTE_REMOTE id=" << route.routeId << " target=" << route.targetNode;
    return RouteStatus::Sent;
}

void ChatService::storeOffline(int senderId, int receiverId, const json& payload)
{
    const auto encoded = payload.dump();
    DBThreadPool::instance()->submitVoid([this, senderId, receiverId, encoded]() {
        _chatHistoryModel.insertOneChat(senderId, receiverId, encoded, 2);
    });
}

void ChatService::handleRoutedMessage(cluster::RoutedMessage message)
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
        return;
    }
    // Subscriber thread only parses, validates and locates a Session; all further
    // owner checks/encoding occur on a Muduo loop, never against foreign context.
    auto session = _sessionManager.sessionForUser(message.receiverUserId);
    auto connection = session ? session->connection() : nullptr;
    auto* loop = connection ? connection->getLoop() : _mainLoop.load();
    if (!loop) { ++_routeFailed; return; }
    loop->queueInLoop([this, message = std::move(message)]() mutable {
        processRoutedMessage(std::move(message));
    });
}

void ChatService::processRoutedMessage(cluster::RoutedMessage message)
{
    auto owner = _presence.lookupUser(message.receiverUserId);
    auto session = _sessionManager.sessionForUser(message.receiverUserId);
    if (owner.status == cluster::LookupStatus::Hit &&
        owner.owner.nodeId == _presence.nodeId() && session &&
        owner.owner.sessionId == session->id() &&
        _presence.ownsUser(message.receiverUserId, session->id()))
    {
        if (message.messageType == ONE_CHAT_MSG)
            deliverRoutedOneChat(session, message.payload);
        else if (auto target = session->connection())
            ChatServer::sendResponse(target, message.payload);
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
        if (_route.send(message)) { ++_routeRemote; return; }
    }
    // Pub/Sub has no server-to-server ACK. A stale route cannot claim delivery.
    ++_routeFailed;
}

void ChatService::deliverRoutedOneChat(
    const std::shared_ptr<const gateway::Session>& session, const json& payload)
{
    auto target = session ? session->connection() : nullptr;
    if (!target || !target->connected()) { ++_routeFailed; return; }
    if (target->getLoop() != muduo::net::EventLoop::getEventLoopOfCurrentThread())
    {
        std::weak_ptr<const gateway::Session> weak = session;
        target->getLoop()->queueInLoop([this, weak, payload]() {
            if (auto current = weak.lock()) deliverRoutedOneChat(current, payload);
        });
        return;
    }
    if (_sessionManager.sessionForUser(session->userId()) != session ||
        !_presence.ownsUser(session->userId(), session->id()))
    {
        ++_routeStale;
        return;
    }
    const auto* state = boost::any_cast<protocol::ProtocolState>(&target->getContext());
    if (state && state->selected() == protocol::WireProtocol::Binary)
    {
        ChatServer::sendResponse(target, payload);
        return;
    }
    const auto deliveryId = _pendingDeliveries.nextDeliveryId();
    gateway::MessageEnvelope delivery(ONE_CHAT_MSG, payload,
                                      protocol::WireProtocol::Undetermined, deliveryId);
    if (!_pendingDeliveries.track(session, delivery))
    {
        ++_routeFailed;
        LOG_WARN << "ROUTE_FAILED pending capacity user=" << session->userId();
        return;
    }
    LOG_INFO << "ROUTE_DELIVERY route_target=" << _presence.nodeId()
             << " delivery_id=" << deliveryId;
    ChatServer::sendResponse(target, delivery);
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
    const auto result = _pendingDeliveries.ack(*deliveryId, conn);
    if (result == gateway::AckResult::Acked)
    {
        LOG_DEBUG << "DELIVERY_ACK id=" << *deliveryId;
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

void ChatService::scanReliableDeliveries()
{
    for (const auto& retry : _pendingDeliveries.scanExpired())
    {
        auto session = retry.session.lock();
        if (!session || _sessionManager.sessionForUser(retry.receiverId) != session)
            continue;
        auto connection = session->connection();
        if (!connection || !connection->connected())
            continue;
        if (!_presence.ownsUser(retry.receiverId, session->id()))
        {
            _pendingDeliveries.cancelConnection(connection);
            continue;
        }
        LOG_DEBUG << "DELIVERY_TIMEOUT id=" << *retry.envelope.deliveryId();
        LOG_DEBUG << "DELIVERY_RETRY id=" << *retry.envelope.deliveryId()
                  << " count=" << retry.retryCount;
        ChatServer::sendResponse(connection, retry.envelope);
    }
}

void ChatService::heartbeatPresence()
{
    if (!_presence.heartbeat(_sessionManager.activeSessions()))
        LOG_ERROR << "PRESENCE_HEARTBEAT failed node=" << _presence.nodeId();
}

void ChatService::logClusterMetrics() const
{
    const auto presence = _presence.metrics();
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
             << " node_heartbeat=" << presence.nodeHeartbeat;
}

void ChatService::logDeliveryMetrics() const
{
    const auto metrics = _pendingDeliveries.metrics();
    LOG_INFO << "DELIVERY_METRICS sent=" << metrics.sent
             << " acked=" << metrics.acked
             << " retried=" << metrics.retried
             << " timeout=" << metrics.timedOut
             << " failed=" << metrics.failed
             << " pending=" << metrics.pending
             << " peak=" << metrics.pendingPeak
             << " pending_bytes=" << metrics.pendingBytes
             << " peak_bytes=" << metrics.pendingPeakBytes;
}
