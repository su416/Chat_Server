#include "chathistorymodel.hpp"
#include "db.h"
#include "connectionpool.h"
#include "json.hpp"
#include "public.hpp"
#include <cstdio>
#include <cstring>

using json = nlohmann::json;

// 存储单聊消息（status: 0=已送达, 2=离线待投递）
void ChatHistoryModel::insertOneChat(int fromid, int toid, const string& msg,
                                     int status, const string& extra)
{
    auto mysql = ConnectionPool::instance()->getConnection();

    const string sql =
        "insert into chatmessage(fromid, toid, groupid, msg, status, extra) "
        "values(?, ?, null, ?, ?, ?)";

    unsigned long msgLen = static_cast<unsigned long>(msg.size());
    unsigned long extraLen = static_cast<unsigned long>(extra.size());

    MYSQL_BIND bind[5];
    memset(bind, 0, sizeof(bind));

    bind[0].buffer_type = MYSQL_TYPE_LONG;
    bind[0].buffer = reinterpret_cast<char*>(&fromid);

    bind[1].buffer_type = MYSQL_TYPE_LONG;
    bind[1].buffer = reinterpret_cast<char*>(&toid);

    bind[2].buffer_type = MYSQL_TYPE_STRING;
    bind[2].buffer = const_cast<char*>(msg.c_str());
    bind[2].buffer_length = msgLen;
    bind[2].length = &msgLen;

    bind[3].buffer_type = MYSQL_TYPE_TINY;
    bind[3].buffer = reinterpret_cast<char*>(&status);

    bind[4].buffer_type = MYSQL_TYPE_STRING;
    bind[4].buffer = extra.empty() ? nullptr : const_cast<char*>(extra.c_str());
    bind[4].buffer_length = extraLen;
    bind[4].length = &extraLen;
    static bool nullFlag = true;
    bind[4].is_null = extra.empty() ? &nullFlag : nullptr;

    mysql->executePrepared(sql, bind, 5);
}

// 存储群聊消息
void ChatHistoryModel::insertGroupChat(int fromid, int groupid, const string& msg,
                                       int status, const string& extra)
{
    auto mysql = ConnectionPool::instance()->getConnection();

    const string sql =
        "insert into chatmessage(fromid, toid, groupid, msg, status, extra) "
        "values(?, null, ?, ?, ?, ?)";

    unsigned long msgLen = static_cast<unsigned long>(msg.size());
    unsigned long extraLen = static_cast<unsigned long>(extra.size());

    MYSQL_BIND bind[5];
    memset(bind, 0, sizeof(bind));

    bind[0].buffer_type = MYSQL_TYPE_LONG;
    bind[0].buffer = reinterpret_cast<char*>(&fromid);

    bind[1].buffer_type = MYSQL_TYPE_LONG;
    bind[1].buffer = reinterpret_cast<char*>(&groupid);

    bind[2].buffer_type = MYSQL_TYPE_STRING;
    bind[2].buffer = const_cast<char*>(msg.c_str());
    bind[2].buffer_length = msgLen;
    bind[2].length = &msgLen;

    bind[3].buffer_type = MYSQL_TYPE_TINY;
    bind[3].buffer = reinterpret_cast<char*>(&status);

    bind[4].buffer_type = MYSQL_TYPE_STRING;
    bind[4].buffer = extra.empty() ? nullptr : const_cast<char*>(extra.c_str());
    bind[4].buffer_length = extraLen;
    bind[4].length = &extraLen;
    static bool nullFlag = true;
    bind[4].is_null = extra.empty() ? &nullFlag : nullptr;

    mysql->executePrepared(sql, bind, 5);
}

// 查询某个用户最近的单聊历史消息
vector<string> ChatHistoryModel::queryRecentOneChat(int userid, int limit)
{
    char sql[2048] = {0};
    snprintf(sql, sizeof(sql),
             "select id, fromid, toid, msg, send_time "
             "from chatmessage "
             "where groupid is null and (fromid = %d or toid = %d) "
             "order by id desc limit %d",
             userid, userid, limit);

    vector<string> vec;
    auto mysql = ConnectionPool::instance()->getConnection();
    MYSQL_RES *res = mysql->query(sql);
    if (res != nullptr)
    {
        MYSQL_ROW row;
        while ((row = mysql_fetch_row(res)) != nullptr)
        {
            json js;
            js["id"] = atoll(row[0]);
            js["fromid"] = atoi(row[1]);
            js["toid"] = atoi(row[2]);
            js["msg"] = row[3];
            js["send_time"] = row[4];
            vec.push_back(js.dump());
        }
        mysql_free_result(res);
    }
    return vec;
}

// 查询某个群最近的群聊历史消息
vector<string> ChatHistoryModel::queryRecentGroupChat(int groupid, int limit)
{
    char sql[2048] = {0};
    snprintf(sql, sizeof(sql),
             "select id, fromid, groupid, msg, send_time "
             "from chatmessage "
             "where groupid = %d "
             "order by id desc limit %d",
             groupid, limit);

    vector<string> vec;
    auto mysql = ConnectionPool::instance()->getConnection();
    MYSQL_RES *res = mysql->query(sql);
    if (res != nullptr)
    {
        MYSQL_ROW row;
        while ((row = mysql_fetch_row(res)) != nullptr)
        {
            json js;
            js["id"] = atoll(row[0]);
            js["fromid"] = atoi(row[1]);
            js["groupid"] = atoi(row[2]);
            js["msg"] = row[3];
            js["send_time"] = row[4];
            vec.push_back(js.dump());
        }
        mysql_free_result(res);
    }
    return vec;
}

// 旧群聊兼容字段；单聊不能从这里提前完成。
vector<string> ChatHistoryModel::queryOffline(int userid)
{
    char sql[512] = {0};
    snprintf(sql, sizeof(sql),
             "select msg from chatmessage "
             "where toid = %d and status = 2 and JSON_VALID(msg) "
             "and JSON_EXTRACT(msg,'$.msgid')=%d order by id",
             userid, GROUP_CHAT_MSG);

    vector<string> vec;
    auto mysql = ConnectionPool::instance()->getConnection();
    MYSQL_RES *res = mysql->query(sql);
    if (res != nullptr)
    {
        MYSQL_ROW row;
        while ((row = mysql_fetch_row(res)) != nullptr)
        {
            vec.push_back(row[0] ? row[0] : "");
        }
        mysql_free_result(res);
    }
    return vec;
}

// 标记离线消息为已送达
void ChatHistoryModel::markDelivered(int userid)
{
    auto mysql = ConnectionPool::instance()->getConnection();
    char sql[512] = {0};
    snprintf(sql, sizeof(sql), "update chatmessage set status=0 "
             "where toid=%d and status=2 and JSON_VALID(msg) "
             "and JSON_EXTRACT(msg,'$.msgid')=%d",
             userid, GROUP_CHAT_MSG);
    mysql->update(sql);
}
