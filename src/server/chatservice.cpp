#include "chatservice.hpp"
#include "chatserver.hpp"
#include "public.hpp"
#include "jsoncodec.hpp"
#include "password_utils.hpp"
#include "rate_limiter.hpp"
#include "db_thread_pool.hpp"
#include <muduo/base/Logging.h>
#include <vector>
#include "proto_utils.hpp"
#include "config.hpp"

using namespace std;
using namespace muduo;

namespace
{
void sendTcpJson(const TcpConnectionPtr &conn, const json &js)
{
    conn->send(ChatServer::encodeResponse(conn, js.dump()));
}

void sendTcpRaw(const TcpConnectionPtr &conn, const std::string &payload)
{
    // Redis 转发的跨服消息始终用 JSON 编码（内部协议不暴露给客户端）
    conn->send(JsonCodec::encode(payload));
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
    // 用户基本业务管理相关事件处理回调注册
    _msgHandlerMap.insert({LOGIN_MSG, std::bind(&ChatService::login, this, _1, _2, _3)});
    _msgHandlerMap.insert({LOGINOUT_MSG, std::bind(&ChatService::loginout, this, _1, _2, _3)});
    _msgHandlerMap.insert({REG_MSG, std::bind(&ChatService::reg, this, _1, _2, _3)});
    _msgHandlerMap.insert({ONE_CHAT_MSG, std::bind(&ChatService::oneChat, this, _1, _2, _3)});
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
    }
}

// 服务器异常，业务重置方法
void ChatService::reset()
{
    // 把online状态的用户，设置成offline
    _userModel.resetState();
}

// 获取消息对应的处理器
MsgHandler ChatService::getHandler(int msgid)
{
    // 记录错误日志，msgid没有对应的事件处理回调
    auto it = _msgHandlerMap.find(msgid);
    if (it == _msgHandlerMap.end())
    {
        // 返回一个默认的处理器，空操作
        return [=](const TcpConnectionPtr &conn, json &js, Timestamp) {
            LOG_ERROR << "msgid:" << msgid << " can not find handler!";
        };
    }
    else
    {
        return _msgHandlerMap[msgid];
    }
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
            // 登录成功，记录用户连接信息
            {
                lock_guard<shared_mutex> lock(_connMutex);
                _userConnMap.insert({id, conn});
            }

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

            // 查询该用户是否有离线消息
            vector<string> vec = _offlineMsgModel.query(id);
            if (!vec.empty())
            {
                response["offlinemsg"] = vec;
                // 读取该用户的离线消息后，把该用户的所有离线消息删除掉
                _offlineMsgModel.remove(id);
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

    {
        lock_guard<shared_mutex> lock(_connMutex);
        auto it = _userConnMap.find(userid);
        if (it != _userConnMap.end())
        {
            _userConnMap.erase(it);
        }
    }

    // 用户注销，相当于就是下线，在redis中取消订阅通道
    _redis.unsubscribe(userid);

    // 更新用户的状态信息
    User user(userid, "", "", "offline");
    _userModel.updateState(user);
}

// 处理客户端异常退出
void ChatService::clientCloseException(const TcpConnectionPtr &conn)
{
    User user;
    {
        lock_guard<shared_mutex> lock(_connMutex);
        for (auto it = _userConnMap.begin(); it != _userConnMap.end(); ++it)
        {
            if (it->second == conn)
            {
                // 从map表删除用户的链接信息
                user.setId(it->first);
                _userConnMap.erase(it);
                break;
            }
        }
    }

    // 用户注销，相当于就是下线，在redis中取消订阅通道
    _redis.unsubscribe(user.getId());

    // 更新用户的状态信息
    if (user.getId() != -1)
    {
        user.setState("offline");
        _userModel.updateState(user);
    }
}

// 一对一聊天业务
void ChatService::oneChat(const TcpConnectionPtr &conn, json &js, Timestamp time)
{           
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

    // 异步落库单聊历史消息（不阻塞 IO 线程）
    DBThreadPool::instance()->submitVoid([this, fromid, toid, msg]() {
        _chatHistoryModel.insertOneChat(fromid, toid, msg);
    });

    {
        shared_lock<shared_mutex> lock(_connMutex);
        auto it = _userConnMap.find(toid);
        if (it != _userConnMap.end())
        {
            // toid 在线，转发消息
            sendTcpJson(it->second, js);
            return;
        }
    }

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

    // toid 不在线（或 Redis 不可用），异步存储离线消息
    {
        string msgDump = js.dump();
        DBThreadPool::instance()->submitVoid([this, toid, msgDump]() {
            _offlineMsgModel.insert(toid, msgDump);
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
        {
            shared_lock<shared_mutex> lock(_connMutex);
            auto it = _userConnMap.find(friendid);
            if (it != _userConnMap.end())
            {
                sendTcpJson(it->second, notify);
                return;
            }
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

    // 异步落库群聊历史消息（不阻塞 IO 线程）
    DBThreadPool::instance()->submitVoid([this, userid, groupid, msg]() {
        _chatHistoryModel.insertGroupChat(userid, groupid, msg);
    });

    vector<int> useridVec = _groupModel.queryGroupUsers(userid, groupid);

    // 读锁：快速扫描 map，收集在线连接和离线 ID
    vector<TcpConnectionPtr> onlineConns;
    vector<int> offlineIds;
    {
        shared_lock<shared_mutex> lock(_connMutex);
        for (int id : useridVec)
        {
            auto it = _userConnMap.find(id);
            if (it != _userConnMap.end())
            {
                onlineConns.push_back(it->second);
            }
            else
            {
                offlineIds.push_back(id);
            }
        }
    }
    // 锁已释放，下面的 DB 操作不阻塞其他线程读取 _userConnMap

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
                    DBThreadPool::instance()->submitVoid([this, id, msgDump]() {
                        _offlineMsgModel.insert(id, msgDump);
                    });
                }
            }
            else
            {
                // 异步存储离线群消息
                string msgDump = js.dump();
                DBThreadPool::instance()->submitVoid([this, id, msgDump]() {
                    _offlineMsgModel.insert(id, msgDump);
                });
            }
        }
    }
}

// 从redis消息队列中获取订阅的消息
void ChatService::handleRedisSubscribeMessage(int userid, string msg)
{
    shared_lock<shared_mutex> lock(_connMutex);
    auto it = _userConnMap.find(userid);
    if (it != _userConnMap.end())
    {
        sendTcpRaw(it->second, msg);
        return;
    }

    // 存储该用户的离线消息
    _offlineMsgModel.insert(userid, msg);
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