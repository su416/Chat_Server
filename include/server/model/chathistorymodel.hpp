#ifndef CHATHISTORYMODEL_H
#define CHATHISTORYMODEL_H

#include <string>
#include <vector>
using namespace std;

// 聊天记录表操作类
class ChatHistoryModel
{
public:
    // 存储单聊消息（extra: 发送者昵称等扩展信息 JSON）
    void insertOneChat(int fromid, int toid, const string& msg,
                       int status = 0, const string& extra = "");

    // 存储群聊消息
    void insertGroupChat(int fromid, int groupid, const string& msg,
                         int status = 0, const string& extra = "");

    // 查询某个用户最近的单聊历史消息
    vector<string> queryRecentOneChat(int userid, int limit);

    // 查询某个群最近的群聊历史消息
    vector<string> queryRecentGroupChat(int groupid, int limit);

    // Phase 7: compatibility login field is group-only. Legacy one-chat rows
    // are migrated into pending_delivery and sent with delivery_id + ACK.
    vector<string> queryOffline(int userid);

    // 标记离线消息为已送达
    void markDelivered(int userid);
};

#endif

