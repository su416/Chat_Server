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

void sendTcpRaw(const TcpConnectionPtr &conn, const std::string &payload)
{
    // Redis 内部仍传 JSON；到客户端时按该连接的 wire protocol 重新编码。
    try
    {
        ChatServer::sendResponse(conn, json::parse(payload));
    }
    catch (const std::exception& error)
    {
        LOG_WARN << "invalid internal Redis message: " << error.what();
    }
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

    // 连接redis服务器
    if (_redis.connect())
    {
        // 设置上报消息的回调
        _redis.init_notify_handler(std::bind(&ChatService::handleRedisSubscribeMessage, this, _1, _2));

        // 设置重连后回调：批量恢复所有在线用户的订阅，消除消息丢失窗口
        _redis.setReconnectHandler([this]() {
            const auto userIds = _sessionManager.activeUserIds();
            for (int userid : userIds)
            {
                _redis.subscribe(userid);
            }
        });
    }
}

// 服务器异常，业务重置方法
void ChatService::reset()
{
    // 把online状态的用户，设置成offline
    _userModel.resetState();
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
        if (user.getState() == "online")
        {
            // 该用户已经登录，不允许重复登录
            json response;
            response["msgid"] = LOGIN_MSG_ACK;
            response["errno"] = 2;
            response["errmsg"] = "this account is using, input another!";
            sendTcpJson(conn, response);
        }
        else
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

            syncServerConnections();

            // id用户登录成功后，向redis订阅channel(id)
            _redis.subscribe(id);

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

    // 只允许当前会话对应的连接解绑；旧连接的迟到请求不能删除新会话。
    if (!_sessionManager.unbindUser(userid, conn))
    {
        return;
    }

    const auto cancelled = _pendingDeliveries.cancelConnection(conn);
    if (cancelled)
        LOG_WARN << "DELIVERY_FAILED disconnected session count=" << cancelled;

    syncServerConnections();

    // 用户注销，相当于就是下线，在redis中取消订阅通道
    _redis.unsubscribe(userid);

    // 更新用户的状态信息
    User user(userid, "", "", "offline");
    _userModel.updateState(user);
}

// 处理客户端异常退出
void ChatService::clientCloseException(const TcpConnectionPtr &conn)
{
    const auto cancelled = _pendingDeliveries.cancelConnection(conn);
    if (cancelled)
        LOG_WARN << "DELIVERY_FAILED disconnected session count=" << cancelled;
    auto userId = _sessionManager.unbindConnection(conn);
    if (!userId)
    {
        // 未登录连接断开不触发 Redis 退订或数据库状态更新。
        return;
    }

    syncServerConnections();

    // 用户注销，相当于就是下线，在redis中取消订阅通道
    _redis.unsubscribe(*userId);

    // 更新用户的状态信息
    User user(*userId, "", "", "offline");
    _userModel.updateState(user);
}

// 一对一聊天业务
void ChatService::oneChat(const TcpConnectionPtr &conn, json &js, Timestamp time)
{
    // delivery_id 只能由服务端为本地在线投递生成，不能接受发送者自报身份。
    js.erase("delivery_id");
    int fromid = js["id"].get<int>();
    int toid = js["toid"].get<int>();
    string msg = js["msg"];

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
        if (auto targetConnection = session->connection())
        {
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
                        _sessionManager.sessionForUser(toid) != currentSession)
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

    // 查询 toid 是否在线
    User user = _userModel.query(toid);
    if (user.getState() == "online")
    {
        if (_redis.publish(toid, js.dump()))
        {
            return;
        }
        // Redis 不可用，降级为本地离线存储
        LOG_ERROR << "redis publish failed, fallback to offline storage";
    }

    // toid 不在线（或 Redis 不可用），异步存储离线消息到 chatmessage
    {
        string msgDump = js.dump();
        DBThreadPool::instance()->submitVoid([this, fromid, toid, msgDump]() {
            _chatHistoryModel.insertOneChat(fromid, toid, msgDump, 2);
        });
    }
}

// 添加好友业务
void ChatService::addFriend(const TcpConnectionPtr &conn, json &js, Timestamp time)
{
    int userid = js["id"].get<int>();
    int friendid = js["friendid"].get<int>();

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
        if (auto friendConnection = _sessionManager.connectionForUser(friendid))
        {
            sendTcpJson(friendConnection, notify);
            return;
        }

        // 跨服在线 → Redis 推送
        if (friendUser.getState() == "online")
        {
            _redis.publish(friendid, notify.dump());
        }
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

    // 获取路由快照，收集本地在线连接和非本地用户 ID。
    vector<int> offlineIds;
    vector<TcpConnectionPtr> onlineConns =
        _sessionManager.connectionsForUsers(useridVec, offlineIds);
    // SessionManager 锁已释放，下面的网络和 DB 操作不会阻塞会话读写。

    // 转发给在线成员
    for (auto &c : onlineConns)
    {
        sendTcpJson(c, js);
    }

    // 处理离线成员：一次批量查询所有状态（消除 N+1）
    if (!offlineIds.empty())
    {
        auto stateMap = _userModel.queryStates(offlineIds);
        for (int id : offlineIds)
        {
            string state = stateMap[id];
            if (state == "online")
            {
                if (!_redis.publish(id, js.dump()))
                {
                    // Redis 不可用，降级为本地离线存储（异步）
                    string msgDump = js.dump();
                    DBThreadPool::instance()->submitVoid([this, userid, id, msgDump]() {
                        _chatHistoryModel.insertOneChat(userid, id, msgDump, 2);
                    });
                }
            }
            else
            {
                // 异步存储离线群消息到 chatmessage（toid=接收者）
                string msgDump = js.dump();
                DBThreadPool::instance()->submitVoid([this, userid, id, msgDump]() {
                    _chatHistoryModel.insertOneChat(userid, id, msgDump, 2);
                });
            }
        }
    }
}

// 从redis消息队列中获取订阅的消息
void ChatService::handleRedisSubscribeMessage(int userid, string msg)
{
    if (auto targetConnection = _sessionManager.connectionForUser(userid))
    {
        sendTcpRaw(targetConnection, msg);
        return;
    }

    // 存储该用户的离线消息到 chatmessage
    _chatHistoryModel.insertOneChat(0, userid, msg, 2);
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
        LOG_DEBUG << "DELIVERY_TIMEOUT id=" << *retry.envelope.deliveryId();
        LOG_DEBUG << "DELIVERY_RETRY id=" << *retry.envelope.deliveryId()
                  << " count=" << retry.retryCount;
        ChatServer::sendResponse(connection, retry.envelope);
    }
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
