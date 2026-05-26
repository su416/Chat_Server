#ifndef CHATHISTORYMODEL_H
#define CHATHISTORYMODEL_H

#include <string>
#include <vector>
using namespace std;

// 聊天记录表操作类
class ChatHistoryModel
{
public:
    // 存储单聊消息（status: 0=已送达, 2=离线待投递）
    void insertOneChat(int fromid, int toid, const string& msg,
                       int status = 0);

    // 存储群聊消息
    void insertGroupChat(int fromid, int groupid, const string& msg,
                         int status = 0);

    // 查询某个用户最近的单聊历史消息
    vector<string> queryRecentOneChat(int userid, int limit);

    // 查询某个群最近的群聊历史消息
    vector<string> queryRecentGroupChat(int groupid, int limit);

    // 查询离线消息（status=2, 用于 login 推送）
    vector<string> queryOffline(int userid);

    // 标记离线消息为已送达
    void markDelivered(int userid);
};

#endif

