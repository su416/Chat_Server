#ifndef GROUPMODEL_H
#define GROUPMODEL_H

#include "group.hpp"
#include <string>
#include <vector>
using namespace std;

// 维护群组信息的操作接口方法
class GroupModel
{
public:
    // 创建群组
    bool createGroup(Group &group);

    // 加入群组
    bool addGroup(int userid, int groupid, string role);

    // 查询用户所在群组信息
    vector<Group> queryGroups(int userid);

    // 根据指定的groupid查询群组用户id列表，除userid自己，主要用于群聊业务给群组其它成员群发消息
    vector<int> queryGroupUsers(int userid, int groupid);

    // 根据 groupid 查询群组基本信息
    Group queryGroup(int groupid);

    // 根据 groupname 查询群组基本信息
    Group queryGroupByName(const string &groupname);

    // 判断用户是否已经在群里
    bool isUserInGroup(int userid, int groupid);
};

#endif