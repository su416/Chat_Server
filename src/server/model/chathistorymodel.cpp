#include "chathistorymodel.hpp"
#include "db.h"
#include "connectionpool.h"
#include "json.hpp"
#include <cstdio>
#include <cstring>

using json = nlohmann::json;

// 存储单聊消息
void ChatHistoryModel::insertOneChat(int fromid, int toid, const string& msg)
{
    auto mysql = ConnectionPool::instance()->getConnection();

    const string sql =
        "insert into chatmessage(fromid, toid, groupid, msg) values(?, ?, null, ?)";

    MYSQL_BIND bind[3];
    memset(bind, 0, sizeof(bind));

    // 参数1：fromid
    bind[0].buffer_type = MYSQL_TYPE_LONG;
    bind[0].buffer = reinterpret_cast<char*>(&fromid);

    // 参数2：toid
    bind[1].buffer_type = MYSQL_TYPE_LONG;
    bind[1].buffer = reinterpret_cast<char*>(&toid);

    // 参数3：msg
    unsigned long msgLength = static_cast<unsigned long>(msg.size());
    bind[2].buffer_type = MYSQL_TYPE_STRING;
    bind[2].buffer = const_cast<char*>(msg.c_str());
    bind[2].buffer_length = msgLength;
    bind[2].length = &msgLength;

    mysql->executePrepared(sql, bind, 3);
}

// 存储群聊消息
void ChatHistoryModel::insertGroupChat(int fromid, int groupid, const string& msg)
{
    auto mysql = ConnectionPool::instance()->getConnection();

    const string sql =
        "insert into chatmessage(fromid, toid, groupid, msg) values(?, null, ?, ?)";

    MYSQL_BIND bind[3];
    memset(bind, 0, sizeof(bind));

    // 参数1：fromid
    bind[0].buffer_type = MYSQL_TYPE_LONG;
    bind[0].buffer = reinterpret_cast<char*>(&fromid);

    // 参数2：groupid
    bind[1].buffer_type = MYSQL_TYPE_LONG;
    bind[1].buffer = reinterpret_cast<char*>(&groupid);

    // 参数3：msg
    unsigned long msgLength = static_cast<unsigned long>(msg.size());
    bind[2].buffer_type = MYSQL_TYPE_STRING;
    bind[2].buffer = const_cast<char*>(msg.c_str());
    bind[2].buffer_length = msgLength;
    bind[2].length = &msgLength;

    mysql->executePrepared(sql, bind, 3);
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