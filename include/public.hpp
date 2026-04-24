#ifndef PUBLIC_H
#define PUBLIC_H

/*
server 和 client 的公共文件
*/
enum EnMsgType
{
    LOGIN_MSG = 1,      // 登录消息
    LOGIN_MSG_ACK,      // 登录响应消息
    LOGINOUT_MSG,       // 注销消息
    REG_MSG,            // 注册消息
    REG_MSG_ACK,        // 注册响应消息
    ONE_CHAT_MSG,       // 聊天消息
    ADD_FRIEND_MSG,     // 添加好友消息

    CREATE_GROUP_MSG,   // 创建群组
    ADD_GROUP_MSG,      // 加入群组
    GROUP_CHAT_MSG,     // 群聊天

    QUERY_ONE_CHAT_HISTORY_MSG,      // 查询单聊历史消息
    QUERY_ONE_CHAT_HISTORY_MSG_ACK,  // 查询单聊历史响应
    QUERY_GROUP_CHAT_HISTORY_MSG,    // 查询群聊历史消息
    QUERY_GROUP_CHAT_HISTORY_MSG_ACK, // 查询群聊历史响应
    ADD_FRIEND_MSG_ACK, //添加好友成功
CREATE_GROUP_MSG_ACK,  //创建群成功
ADD_GROUP_MSG_ACK  //加群成功
};

#endif
