#ifndef PROTO_UTILS_HPP
#define PROTO_UTILS_HPP

#include "chat.pb.h"
#include "json.hpp"
#include <string>
#include <vector>

using json = nlohmann::json;
using namespace std;

namespace ProtoUtils
{
    inline chat::ChatHistoryItem parseHistoryItem(const string& item)
    {
        json js = json::parse(item);

        chat::ChatHistoryItem pbItem;
        if (js.contains("id")) pbItem.set_id(js["id"].get<long long>());
        if (js.contains("fromid")) pbItem.set_fromid(js["fromid"].get<int>());
        if (js.contains("toid")) pbItem.set_toid(js["toid"].get<int>());
        if (js.contains("groupid")) pbItem.set_groupid(js["groupid"].get<int>());
        if (js.contains("msg")) pbItem.set_msg(js["msg"].get<string>());
        if (js.contains("send_time")) pbItem.set_send_time(js["send_time"].get<string>());

        return pbItem;
    }

    inline chat::QueryOneChatHistoryResponse buildOneChatHistoryResponse(
        int userid,
        const vector<string>& historyVec)
    {
        chat::QueryOneChatHistoryResponse response;
        response.set_errcode(0);
        response.set_id(userid);

        for (const auto& item : historyVec)
        {
            auto* pbItem = response.add_history();
            *pbItem = parseHistoryItem(item);
        }

        return response;
    }

    inline chat::QueryGroupChatHistoryResponse buildGroupChatHistoryResponse(
        int groupid,
        const vector<string>& historyVec)
    {
        chat::QueryGroupChatHistoryResponse response;
        response.set_errcode(0);
        response.set_groupid(groupid);

        for (const auto& item : historyVec)
        {
            auto* pbItem = response.add_history();
            *pbItem = parseHistoryItem(item);
        }

        return response;
    }
}

#endif
