#include "chathistorymodel.hpp"
#include "db.h"
#include "json.hpp"
#include <cstdio>
#include <memory>

using json = nlohmann::json;

// 存储单聊消息
void ChatHistoryModel::insertOneChat(int fromid, int toid, const string& msg)
{
    MySQL mysql;
    if (!mysql.connect())
    {
        return;
    }

    // 对消息内容做转义，避免单引号等特殊字符破坏 SQL
    unique_ptr<char[]> escapeMsg(new char[msg.size() * 2 + 1]);
    mysql_real_escape_string(mysql.getConnection(), escapeMsg.get(), msg.c_str(), msg.size());

    char sql[4096] = {0};
    snprintf(sql, sizeof(sql),
             "insert into chatmessage(fromid, toid, groupid, msg) values(%d, %d, null, '%s')",
             fromid, toid, escapeMsg.get());

    mysql.update(sql);
}

// 存储群聊消息
void ChatHistoryModel::insertGroupChat(int fromid, int groupid, const string& msg)
{
    MySQL mysql;
    if (!mysql.connect())
    {
        return;
    }

    // 对消息内容做转义，避免单引号等特殊字符破坏 SQL
    unique_ptr<char[]> escapeMsg(new char[msg.size() * 2 + 1]);
    mysql_real_escape_string(mysql.getConnection(), escapeMsg.get(), msg.c_str(), msg.size());

    char sql[4096] = {0};
    snprintf(sql, sizeof(sql),
             "insert into chatmessage(fromid, toid, groupid, msg) values(%d, null, %d, '%s')",
             fromid, groupid, escapeMsg.get());

    mysql.update(sql);
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
    MySQL mysql;
    if (mysql.connect())
    {
        MYSQL_RES *res = mysql.query(sql);
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
    MySQL mysql;
    if (mysql.connect())
    {
        MYSQL_RES *res = mysql.query(sql);
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
    }
    return vec;
}