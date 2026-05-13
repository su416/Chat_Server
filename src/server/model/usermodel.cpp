#include "usermodel.hpp"
#include "db.h"
#include "connectionpool.h"
#include "password_utils.hpp"
#include <iostream>
using namespace std;

// User表的增加方法
bool UserModel::insert(User &user)
{
    // 1.组装sql语句（密码做 SHA-256 哈希后存储）
    char sql[1024] = {0};
    string hashedPwd = PasswordUtils::hashPassword(user.getPwd());
    sprintf(sql, "insert into user(name, password, state) values('%s', '%s', '%s')",
            user.getName().c_str(), hashedPwd.c_str(), user.getState().c_str());

    auto mysql = ConnectionPool::instance()->getConnection();
    if (mysql->update(sql))
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
    // 1.组装sql语句
    char sql[1024] = {0};
    sprintf(sql, "update user set state = '%s' where id = %d", user.getState().c_str(), user.getId());

    auto mysql = ConnectionPool::instance()->getConnection();
    if (mysql->update(sql))
    {
        return true;
    }
    return false;
}

// 重置用户的状态信息
void UserModel::resetState()
{
    // 1.组装sql语句
    char sql[1024] = "update user set state = 'offline' where state = 'online'";

    auto mysql = ConnectionPool::instance()->getConnection();
    mysql->update(sql);
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