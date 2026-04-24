#include "offlinemessagemodel.hpp"
#include "db.h"
#include <memory>
#include <cstdio>

// 存储用户的离线消息
void OfflineMsgModel::insert(int userid, string msg)
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
             "insert into offlinemessage values(%d, '%s')",
             userid, escapeMsg.get());

    mysql.update(sql);
}

// 删除用户的离线消息
void OfflineMsgModel::remove(int userid)
{
    char sql[1024] = {0};
    snprintf(sql, sizeof(sql), "delete from offlinemessage where userid=%d", userid);

    MySQL mysql;
    if (mysql.connect())
    {
        mysql.update(sql);
    }
}

// 查询用户的离线消息
vector<string> OfflineMsgModel::query(int userid)
{
    char sql[1024] = {0};
    snprintf(sql, sizeof(sql), "select message from offlinemessage where userid = %d", userid);

    vector<string> vec;
    MySQL mysql;
    if (mysql.connect())
    {
        MYSQL_RES *res = mysql.query(sql);
        if (res != nullptr)
        {
            MYSQL_ROW row;
            while((row = mysql_fetch_row(res)) != nullptr)
            {
                vec.push_back(row[0]);
            }
            mysql_free_result(res);
            return vec;
        }
    }
    return vec;
}