#ifndef CHATHISTORYMODEL_H
#define CHATHISTORYMODEL_H

#include <string>
#include <vector>
using namespace std;

// 聊天记录表操作类
class ChatHistoryModel
{
public:
    // 存储单聊消息
    void insertOneChat(int fromid, int toid, const string& msg);

    // 存储群聊消息
    void insertGroupChat(int fromid, int groupid, const string& msg);

    // 查询某个用户最近的单聊历史消息
    vector<string> queryRecentOneChat(int userid, int limit);

    // 查询某个群最近的群聊历史消息
    vector<string> queryRecentGroupChat(int groupid, int limit);
};

#endif

