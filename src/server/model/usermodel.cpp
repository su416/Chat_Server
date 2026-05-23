#include "usermodel.hpp"
#include "db.h"
#include "connectionpool.h"
#include "password_utils.hpp"
#include <cstring>
#include <iostream>
using namespace std;

// User表的增加方法
bool UserModel::insert(User &user)
{
    const string sql = "insert into user(name, password, state) values(?, ?, ?)";

    string name = user.getName();
    string hashedPwd = PasswordUtils::hashPassword(user.getPwd());
    string state = user.getState();

    MYSQL_BIND bind[3];
    memset(bind, 0, sizeof(bind));

    bind[0].buffer_type = MYSQL_TYPE_STRING;
    bind[0].buffer = const_cast<char*>(name.c_str());
    bind[0].buffer_length = static_cast<unsigned long>(name.size());

    bind[1].buffer_type = MYSQL_TYPE_STRING;
    bind[1].buffer = const_cast<char*>(hashedPwd.c_str());
    bind[1].buffer_length = static_cast<unsigned long>(hashedPwd.size());

    bind[2].buffer_type = MYSQL_TYPE_STRING;
    bind[2].buffer = const_cast<char*>(state.c_str());
    bind[2].buffer_length = static_cast<unsigned long>(state.size());

    auto mysql = ConnectionPool::instance()->getConnection();
    if (mysql->executePrepared(sql, bind, 3))
    {
        // 获取插入成功的用户数据生成的主键id
        user.setId(mysql_insert_id(mysql->getConnection()));
        return true;
    }

    return false;
}

// 根据用户号码查询用户信息
User UserModel::query(int id)
{
    // 1.组装sql语句
    char sql[1024] = {0};
    sprintf(sql, "select * from user where id = %d", id);

    auto mysql = ConnectionPool::instance()->getConnection();
    MYSQL_RES *res = mysql->query(sql);
    if (res != nullptr)
    {
        MYSQL_ROW row = mysql_fetch_row(res);
        if (row != nullptr)
        {
            User user;
            user.setId(atoi(row[0]));
            user.setName(row[1]);
            user.setPwd(row[2]);
            user.setState(row[3]);
            mysql_free_result(res);
            return user;
        }
    }

    return User();
}

// 更新用户的状态信息
bool UserModel::updateState(User user)
{
    const string sql = "update user set state = ? where id = ?";

    string state = user.getState();
    int userId = user.getId();

    MYSQL_BIND bind[2];
    memset(bind, 0, sizeof(bind));

    bind[0].buffer_type = MYSQL_TYPE_STRING;
    bind[0].buffer = const_cast<char*>(state.c_str());
    bind[0].buffer_length = static_cast<unsigned long>(state.size());

    bind[1].buffer_type = MYSQL_TYPE_LONG;
    bind[1].buffer = reinterpret_cast<char*>(&userId);
    bind[1].is_unsigned = false;

    auto mysql = ConnectionPool::instance()->getConnection();
    return mysql->executePrepared(sql, bind, 2);
}

// 重置用户的状态信息
void UserModel::resetState()
{
    const string sql = "update user set state = ? where state = ?";

    string offline = "offline";
    string online = "online";

    MYSQL_BIND bind[2];
    memset(bind, 0, sizeof(bind));

    bind[0].buffer_type = MYSQL_TYPE_STRING;
    bind[0].buffer = const_cast<char*>(offline.c_str());
    bind[0].buffer_length = static_cast<unsigned long>(offline.size());

    bind[1].buffer_type = MYSQL_TYPE_STRING;
    bind[1].buffer = const_cast<char*>(online.c_str());
    bind[1].buffer_length = static_cast<unsigned long>(online.size());

    auto mysql = ConnectionPool::instance()->getConnection();
    mysql->executePrepared(sql, bind, 2);
}

// 批量查询用户状态（群聊扇出优化：N 次单独查询 → 1 次批量查询）
unordered_map<int, string> UserModel::queryStates(const vector<int>& ids)
{
    unordered_map<int, string> result;
    if (ids.empty())
        return result;

    // 构造 WHERE id IN (1,2,3,...)
    string sql = "select id, state from user where id in (";
    for (size_t i = 0; i < ids.size(); ++i)
    {
        if (i > 0) sql += ",";
        sql += to_string(ids[i]);
    }
    sql += ")";

    auto mysql = ConnectionPool::instance()->getConnection();
    MYSQL_RES *res = mysql->query(sql);
    if (res != nullptr)
    {
        MYSQL_ROW row;
        while ((row = mysql_fetch_row(res)) != nullptr)
        {
            result[atoi(row[0])] = row[1] ? row[1] : "";
        }
        mysql_free_result(res);
    }
    return result;
}