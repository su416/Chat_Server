#include "offlinemessagemodel.hpp"
#include "db.h"
#include "connectionpool.h"
#include <cstring>

// 存储用户的离线消息
void OfflineMsgModel::insert(int userid, string msg)
{
    auto mysql = ConnectionPool::instance()->getConnection();

    const string sql = "insert into offlinemessage(userid, message) values(?, ?)";

    MYSQL_BIND bind[2];
    memset(bind, 0, sizeof(bind));

    // 参数1：userid
    bind[0].buffer_type = MYSQL_TYPE_LONG;
    bind[0].buffer = reinterpret_cast<char*>(&userid);
    bind[0].is_unsigned = false;

    // 参数2：message
    unsigned long msgLength = static_cast<unsigned long>(msg.size());
    bind[1].buffer_type = MYSQL_TYPE_STRING;
    bind[1].buffer = const_cast<char*>(msg.c_str());
    bind[1].buffer_length = msgLength;
    bind[1].length = &msgLength;

    mysql->executePrepared(sql, bind, 2);
}

// 删除用户的离线消息
void OfflineMsgModel::remove(int userid)
{
    char sql[1024] = {0};
    sprintf(sql, "delete from offlinemessage where userid=%d", userid);

    auto mysql = ConnectionPool::instance()->getConnection();
    mysql->update(sql);
}

// 查询用户的离线消息
vector<string> OfflineMsgModel::query(int userid)
{
    char sql[1024] = {0};
    sprintf(sql, "select message from offlinemessage where userid = %d", userid);

    vector<string> vec;
    auto mysql = ConnectionPool::instance()->getConnection();
    MYSQL_RES *res = mysql->query(sql);
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
    return vec;
}