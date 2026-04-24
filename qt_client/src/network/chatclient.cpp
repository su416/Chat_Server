#include "network/chatclient.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonParseError>
#include <QDebug>

ChatClient::ChatClient(QObject *parent)
    : QObject(parent),
      m_socket(new QTcpSocket(this))
{
    connect(m_socket, &QTcpSocket::connected, this, &ChatClient::onConnected);
    connect(m_socket, &QTcpSocket::disconnected, this, &ChatClient::onDisconnected);
    connect(m_socket, &QTcpSocket::readyRead, this, &ChatClient::onReadyRead);
    connect(m_socket, &QTcpSocket::errorOccurred, this, &ChatClient::onErrorOccurred);
}

void ChatClient::connectToServer(const QString &host, quint16 port)
{
    if (m_socket->state() == QAbstractSocket::ConnectedState)
    {
        return;
    }

    m_socket->connectToHost(host, port);
}

void ChatClient::sendLoginRequest(int userId, const QString &password)
{
    QJsonObject obj;
    obj["msgid"] = 1;       // LOGIN_MSG
    obj["id"] = userId;
    obj["password"] = password;

    sendJson(obj);
}

void ChatClient::sendJson(const QJsonObject &obj)
{
    QByteArray data = QJsonDocument(obj).toJson(QJsonDocument::Compact);

    // 兼容你当前命令行客户端的发送方式：末尾带 '\0'
    data.append('\0');

    qint64 len = m_socket->write(data);
    if (len == -1)
    {
        emit networkError("failed to send data");
    }
}

void ChatClient::onConnected()
{
    emit connected();
}

void ChatClient::onDisconnected()
{
    emit disconnected();
}

void ChatClient::onReadyRead()
{
    QByteArray data = m_socket->readAll();

    while (!data.isEmpty() && data.back() == '\0')
    {
        data.chop(1);
    }

    QJsonParseError error;
    QJsonDocument doc = QJsonDocument::fromJson(data, &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject())
    {
        emit networkError("invalid json response from server");
        return;
    }

    QJsonObject obj = doc.object();
    int msgid = obj.value("msgid").toInt();

    // LOGIN_MSG_ACK = 2
    if (msgid == 2)
    {
        int err = obj.value("errno").toInt();
        if (err == 0)
        {
            emit loginSucceeded(obj);
        }
        else
        {
            QString reason = obj.value("errmsg").toString();
            if (reason.isEmpty())
            {
                reason = "login failed";
            }
            emit loginFailed(reason);
        }
        return;
    }

    // REG_MSG_ACK = 5
    if (msgid == 5)
    {
        int err = obj.value("errno").toInt();
        if (err == 0)
        {
            int id = obj.value("id").toInt();
            emit registerSucceeded(id);
        }
        else
        {
            QString reason = obj.value("errmsg").toString();
            if (reason.isEmpty())
            {
                reason = "register failed";
            }
            emit registerFailed(reason);
        }
        return;
    }

    // ONE_CHAT_MSG = 6
    if (msgid == 6)
    {
        int fromId = obj.value("id").toInt();
        QString name = obj.value("name").toString();
        QString msg = obj.value("msg").toString();
        QString time = obj.value("time").toString();

        emit oneChatMessageReceived(fromId, name, msg, time);
        return;
    }

    // QUERY_ONE_CHAT_HISTORY_MSG_ACK = 12
    if (msgid == 12)
    {
        int err = obj.value("errno").toInt();
        if (err != 0)
        {
            emit networkError("query one chat history failed");
            return;
        }

        QJsonArray history = obj.value("history").toArray();
        emit oneChatHistoryReceived(history);
        return;
    }

    // GROUP_CHAT_MSG = 10
    if (msgid == 10)
    {
        int fromId = obj.value("id").toInt();
        int groupId = obj.value("groupid").toInt();
        QString name = obj.value("name").toString();
        QString msg = obj.value("msg").toString();
        QString time = obj.value("time").toString();

        emit groupChatMessageReceived(fromId, groupId, name, msg, time);
        return;
    }

        // QUERY_GROUP_CHAT_HISTORY_MSG_ACK = 14
    if (msgid == 14)
    {
        int err = obj.value("errno").toInt();
        if (err != 0)
        {
            emit networkError("query group chat history failed");
            return;
        }

        int groupId = obj.value("groupid").toInt();
        QJsonArray history = obj.value("history").toArray();
        emit groupChatHistoryReceived(groupId, history);
        return;
    }

        if (msgid == 15)
    {
        int err = obj.value("errno").toInt();
        if (err == 0)
        {
            emit addFriendSucceeded(
                obj.value("friendid").toInt(),
                obj.value("friendname").toString(),
                obj.value("state").toString()
            );
        }
        else
        {
            emit addFriendFailed(obj.value("errmsg").toString("add friend failed"));
        }
        return;
    }

    if (msgid == 16)
    {
        int err = obj.value("errno").toInt();
        if (err == 0)
        {
            emit createGroupSucceeded(
                obj.value("groupid").toInt(),
                obj.value("groupname").toString(),
                obj.value("groupdesc").toString()
            );
        }
        else
        {
            emit createGroupFailed(obj.value("errmsg").toString("create group failed"));
        }
        return;
    }

    if (msgid == 17)
    {
        int err = obj.value("errno").toInt();
        if (err == 0)
        {
            emit joinGroupSucceeded(
                obj.value("groupid").toInt(),
                obj.value("groupname").toString(),
                obj.value("groupdesc").toString()
            );
        }
        else
        {
            emit joinGroupFailed(obj.value("errmsg").toString("join group failed"));
        }
        return;
    }

    qDebug() << "Unhandled response:" << obj;
}

void ChatClient::onErrorOccurred(QAbstractSocket::SocketError)
{
    emit networkError(m_socket->errorString());
}

void ChatClient::sendRegisterRequest(const QString &name, const QString &password)
{
    QJsonObject obj;
    obj["msgid"] = 4;   // REG_MSG
    obj["name"] = name;
    obj["password"] = password;

    sendJson(obj);
}
void ChatClient::sendOneChatRequest(int userId,
                                    int toId,
                                    const QString &name,
                                    const QString &msg,
                                    const QString &time)
{
    QJsonObject obj;
    obj["msgid"] = 6;   // ONE_CHAT_MSG
    obj["id"] = userId;
    obj["toid"] = toId;
    obj["name"] = name;
    obj["msg"] = msg;
    obj["time"] = time;

    sendJson(obj);
}

void ChatClient::sendQueryOneChatHistoryRequest(int userId, int limit)
{
    QJsonObject obj;
    obj["msgid"] = 11;   // QUERY_ONE_CHAT_HISTORY_MSG
    obj["id"] = userId;
    obj["limit"] = limit;

    sendJson(obj);
}


void ChatClient::sendAddFriendRequest(int userId, int friendId)
{
    QJsonObject obj;
    obj["msgid"] = 7;   // ADD_FRIEND_MSG
    obj["id"] = userId;
    obj["friendid"] = friendId;

    sendJson(obj);
}

void ChatClient::sendCreateGroupRequest(int userId,
                                        const QString &groupName,
                                        const QString &groupDesc)
{
    QJsonObject obj;
    obj["msgid"] = 8;   // CREATE_GROUP_MSG
    obj["id"] = userId;
    obj["groupname"] = groupName;
    obj["groupdesc"] = groupDesc;

    sendJson(obj);
}


void ChatClient::sendJoinGroupRequest(int userId, int groupId)
{
    QJsonObject obj;
    obj["msgid"] = 9;   // ADD_GROUP_MSG
    obj["id"] = userId;
    obj["groupid"] = groupId;

    sendJson(obj);
}


void ChatClient::sendGroupChatRequest(int userId,
                                      int groupId,
                                      const QString &name,
                                      const QString &msg,
                                      const QString &time)
{
    QJsonObject obj;
    obj["msgid"] = 10;   // GROUP_CHAT_MSG
    obj["id"] = userId;
    obj["groupid"] = groupId;
    obj["name"] = name;
    obj["msg"] = msg;
    obj["time"] = time;

    sendJson(obj);
}

void ChatClient::sendQueryGroupChatHistoryRequest(int groupId, int limit)
{
    QJsonObject obj;
    obj["msgid"] = 13;   // QUERY_GROUP_CHAT_HISTORY_MSG
    obj["groupid"] = groupId;
    obj["limit"] = limit;

    sendJson(obj);
}

void ChatClient::sendLogoutRequest(int userId)
{
    QJsonObject obj;
    obj["msgid"] = 3;   // LOGINOUT_MSG
    obj["id"] = userId;

    sendJson(obj);
}

void ChatClient::disconnectFromServer()
{
    if (m_socket->state() != QAbstractSocket::UnconnectedState)
    {
        m_socket->disconnectFromHost();
    }
}

