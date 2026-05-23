#include "groupmodel.hpp"
#include "db.h"
#include "connectionpool.h"
#include <cstring>
using namespace std;

// 创建群组
bool GroupModel::createGroup(Group &group)
{
    const string sql = "insert into allgroup(groupname, groupdesc) values(?, ?)";

    string groupName = group.getName();
    string groupDesc = group.getDesc();

    MYSQL_BIND bind[2];
    memset(bind, 0, sizeof(bind));

    bind[0].buffer_type = MYSQL_TYPE_STRING;
    bind[0].buffer = const_cast<char*>(groupName.c_str());
    bind[0].buffer_length = static_cast<unsigned long>(groupName.size());

    bind[1].buffer_type = MYSQL_TYPE_STRING;
    bind[1].buffer = const_cast<char*>(groupDesc.c_str());
    bind[1].buffer_length = static_cast<unsigned long>(groupDesc.size());

    auto mysql = ConnectionPool::instance()->getConnection();
    if (mysql->executePrepared(sql, bind, 2))
    {
        group.setId(mysql_insert_id(mysql->getConnection()));
        return true;
    }

    return false;
}

// 加入群组
bool GroupModel::addGroup(int userid, int groupid, string role)
{
    const string sql = "insert into groupuser values(?, ?, ?)";

    MYSQL_BIND bind[3];
    memset(bind, 0, sizeof(bind));

    bind[0].buffer_type = MYSQL_TYPE_LONG;
    bind[0].buffer = reinterpret_cast<char*>(&groupid);
    bind[0].is_unsigned = false;

    bind[1].buffer_type = MYSQL_TYPE_LONG;
    bind[1].buffer = reinterpret_cast<char*>(&userid);
    bind[1].is_unsigned = false;

    bind[2].buffer_type = MYSQL_TYPE_STRING;
    bind[2].buffer = const_cast<char*>(role.c_str());
    bind[2].buffer_length = static_cast<unsigned long>(role.size());

    auto mysql = ConnectionPool::instance()->getConnection();
    return mysql->executePrepared(sql, bind, 3);
}

// 查询用户所在群组信息
vector<Group> GroupModel::queryGroups(int userid)
{
    char sql[1024] = {0};
    sprintf(sql, "select a.id,a.groupname,a.groupdesc from allgroup a inner join \
         groupuser b on a.id = b.groupid where b.userid=%d",
            userid);

    vector<Group> groupVec;

    auto mysql = ConnectionPool::instance()->getConnection();
    MYSQL_RES *res = mysql->query(sql);
    if (res != nullptr)
    {
        MYSQL_ROW row;
        while ((row = mysql_fetch_row(res)) != nullptr)
        {
            Group group;
            group.setId(atoi(row[0]));
            group.setName(row[1]);
            group.setDesc(row[2]);
            groupVec.push_back(group);
        }
        mysql_free_result(res);
    }

    for (Group &group : groupVec)
    {
        sprintf(sql, "select a.id,a.name,a.state,b.grouprole from user a \
            inner join groupuser b on b.userid = a.id where b.groupid=%d",
                group.getId());

        MYSQL_RES *res = mysql->query(sql);
        if (res != nullptr)
        {
            MYSQL_ROW row;
            while ((row = mysql_fetch_row(res)) != nullptr)
            {
                GroupUser user;
                user.setId(atoi(row[0]));
                user.setName(row[1]);
                user.setState(row[2]);
                user.setRole(row[3]);
                group.getUsers().push_back(user);
            }
            mysql_free_result(res);
        }
    }
    return groupVec;
}

// 根据指定的groupid查询群组用户id列表，除userid自己
vector<int> GroupModel::queryGroupUsers(int userid, int groupid)
{
    char sql[1024] = {0};
    sprintf(sql, "select userid from groupuser where groupid = %d and userid != %d", groupid, userid);

    vector<int> idVec;
    auto mysql = ConnectionPool::instance()->getConnection();
    MYSQL_RES *res = mysql->query(sql);
    if (res != nullptr)
    {
        MYSQL_ROW row;
        while ((row = mysql_fetch_row(res)) != nullptr)
        {
            idVec.push_back(atoi(row[0]));
        }
        mysql_free_result(res);
    }
    return idVec;
}

// 根据 groupid 查询群组基本信息
Group GroupModel::queryGroup(int groupid)
{
    char sql[1024] = {0};
    sprintf(sql, "select id, groupname, groupdesc from allgroup where id = %d", groupid);

    auto mysql = ConnectionPool::instance()->getConnection();
    MYSQL_RES *res = mysql->query(sql);
    if (res != nullptr)
    {
        MYSQL_ROW row = mysql_fetch_row(res);
        if (row != nullptr)
        {
            Group group;
            group.setId(atoi(row[0]));
            group.setName(row[1]);
            group.setDesc(row[2]);
            mysql_free_result(res);
            return group;
        }
        mysql_free_result(res);
    }

    return Group();
}

// 根据 groupname 查询群组基本信息
Group GroupModel::queryGroupByName(const string &groupname)
{
    char sql[1024] = {0};
    sprintf(sql, "select id, groupname, groupdesc from allgroup where groupname = '%s'",
            groupname.c_str());

    auto mysql = ConnectionPool::instance()->getConnection();
    MYSQL_RES *res = mysql->query(sql);
    if (res != nullptr)
    {
        MYSQL_ROW row = mysql_fetch_row(res);
        if (row != nullptr)
        {
            Group group;
            group.setId(atoi(row[0]));
            group.setName(row[1]);
            group.setDesc(row[2]);
            mysql_free_result(res);
            return group;
        }
        mysql_free_result(res);
    }

    return Group();
}

// 判断用户是否已经在群里
bool GroupModel::isUserInGroup(int userid, int groupid)
{
    char sql[1024] = {0};
    sprintf(sql, "select userid from groupuser where userid = %d and groupid = %d",
            userid, groupid);

    auto mysql = ConnectionPool::instance()->getConnection();
    MYSQL_RES *res = mysql->query(sql);
    if (res != nullptr)
    {
        MYSQL_ROW row = mysql_fetch_row(res);
        bool exists = (row != nullptr);
        mysql_free_result(res);
        return exists;
    }

    return false;
}