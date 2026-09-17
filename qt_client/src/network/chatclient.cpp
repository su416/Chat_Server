#include "network/chatclient.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonParseError>
#include <QDebug>
#include <arpa/inet.h>

// ──── Protobuf Envelope 手动构造（不依赖 protobuf 库）────

static QByteArray protoVarint(quint64 val)
{
    QByteArray out;
    while (val > 127) {
        out.append(static_cast<char>((val & 0x7F) | 0x80));
        val >>= 7;
    }
    out.append(static_cast<char>(val));
    return out;
}

static QByteArray buildEnvelope(int msgid, const QByteArray& payload)
{
    // Envelope { msgid: field1(varint), payload: field2(bytes) }
    QByteArray env;
    // field 1, wire type 0 → tag = (1 << 3) | 0 = 0x08
    env.append('\x08');
    env.append(protoVarint(msgid));
    // field 2, wire type 2 → tag = (2 << 3) | 2 = 0x12
    env.append('\x12');
    env.append(protoVarint(payload.size()));
    env.append(payload);
    return env;
}

// 发送 Protobuf 帧：[4B BE len][Envelope bytes]
void ChatClient::sendProtoFrame(int msgid, const QByteArray& payload)
{
    QByteArray env = buildEnvelope(msgid, payload);
    quint32 netLen = htonl(static_cast<quint32>(env.size()));
    QByteArray frame;
    frame.append(reinterpret_cast<const char*>(&netLen), 4);
    frame.append(env);
    m_socket->write(frame);
}

// 协议协商：发 JSON msgid=0 切换到 Protobuf
void ChatClient::negotiateProto()
{
    QJsonObject req;
    req["msgid"] = 0;
    req["accept"] = "protobuf";
    QByteArray data = QJsonDocument(req).toJson(QJsonDocument::Compact);
    data.append('\0');
    m_socket->write(data);
    // 协商响应由 onReadyRead 按 JSON 帧处理（协商响应固定走 JSON）
}

void ChatClient::setUseProtobuf(bool enabled)
{
    m_useProtobuf = enabled;
}

ChatClient::ChatClient(QObject *parent)
    : QObject(parent),
      m_socket(new QTcpSocket(this))
{
    connect(m_socket, &QTcpSocket::connected, this, &ChatClient::onConnected);
    connect(m_socket, &QTcpSocket::disconnected, this, &ChatClient::onDisconnected);
    connect(m_socket, &QTcpSocket::readyRead, this, &ChatClient::onReadyRead);
    connect(m_socket, &QTcpSocket::errorOccurred, this, &ChatClient::onErrorOccurred);
}

void ChatClient::setAutoReconnect(bool enabled)
{
    m_autoReconnect = enabled;
}

void ChatClient::connectToServer(const QString &host, quint16 port)
{
    m_host = host;
    m_port = port;

    if (m_socket->state() == QAbstractSocket::ConnectedState)
        return;

    m_recvBuf.clear();
    m_socket->connectToHost(host, port);
}

void ChatClient::disconnectFromServer()
{
    m_autoReconnect = false;   // 主动断开时不重连
    if (m_socket->state() != QAbstractSocket::UnconnectedState)
    {
        m_socket->disconnectFromHost();
    }
}

// ---- 内部 ----

void ChatClient::sendJson(const QJsonObject &obj)
{
    if (m_useProtobuf)
    {
        int msgid = obj.value("msgid").toInt();
        QByteArray payload = QJsonDocument(obj).toJson(QJsonDocument::Compact);
        sendProtoFrame(msgid, payload);
    }
    else
    {
        QByteArray data = QJsonDocument(obj).toJson(QJsonDocument::Compact);
        data.append('\0');
        if (m_socket->write(data) == -1)
            emit networkError("failed to send data");
    }
}

// ---- 网络事件 ----

void ChatClient::onConnected()
{
    m_recvBuf.clear();

    if (m_useProtobuf)
        negotiateProto();

    emit connected();
}

void ChatClient::onDisconnected()
{
    emit disconnected();

    // 断线自动重连（指数退避 2s / 4s / 8s，最大 16s）
    if (m_autoReconnect && !m_host.isEmpty() && m_port > 0)
    {
        static int s_retryDelay = 2000;

        QTimer::singleShot(s_retryDelay, this, [this]() {
            if (m_socket->state() == QAbstractSocket::UnconnectedState)
            {
                qDebug() << "reconnecting to" << m_host << m_port
                         << "after" << s_retryDelay << "ms";
                m_socket->connectToHost(m_host, m_port);
            }
        });

        s_retryDelay = qMin(s_retryDelay * 2, 16000);
    }
}

void ChatClient::onReadyRead()
{
    m_recvBuf.append(m_socket->readAll());

    if (m_useProtobuf)
    {
        // Protobuf 路径：[4B BE len][Envelope bytes]
        while (m_recvBuf.size() >= 4)
        {
            quint32 netLen;
            memcpy(&netLen, m_recvBuf.constData(), 4);
            quint32 len = ntohl(netLen);
            if (static_cast<quint32>(m_recvBuf.size()) < 4 + len)
                break;  // 不完整帧

            QByteArray envData = m_recvBuf.mid(4, static_cast<int>(len));
            m_recvBuf.remove(0, 4 + len);

            // 解析 Envelope：跳过 msgid varint + payload tag
            if (envData.isEmpty() || static_cast<unsigned char>(envData[0]) != 0x08)
                continue;
            int pos = 1;
            while (pos < envData.size() && (static_cast<unsigned char>(envData[pos]) & 0x80))
                ++pos;
            ++pos; // 跳过 msgid varint

            // field 2 tag = 0x12
            if (pos >= envData.size() || static_cast<unsigned char>(envData[pos]) != 0x12)
                continue;
            ++pos;

            // payload length (varint)
            quint64 payloadLen = 0;
            int shift = 0;
            while (pos < envData.size()) {
                unsigned char b = envData[pos++];
                payloadLen |= (b & 0x7FULL) << shift;
                if (!(b & 0x80)) break;
                shift += 7;
            }

            QByteArray payload = envData.mid(pos, static_cast<int>(payloadLen));
            QJsonParseError error;
            QJsonDocument doc = QJsonDocument::fromJson(payload, &error);
            if (error.error == QJsonParseError::NoError && doc.isObject())
                dispatchMessage(doc.object());
        }
    }
    else
    {
        // JSON 路径：\0 分帧
        while (true)
        {
            int idx = m_recvBuf.indexOf('\0');
            if (idx == -1) break;

            QByteArray frame = m_recvBuf.left(idx);
            m_recvBuf.remove(0, idx + 1);
            if (frame.isEmpty()) continue;

            QJsonParseError error;
            QJsonDocument doc = QJsonDocument::fromJson(frame, &error);
            if (error.error != QJsonParseError::NoError || !doc.isObject())
            {
                qDebug() << "invalid json frame:" << error.errorString();
                continue;
            }
            dispatchMessage(doc.object());
        }
    }
}

void ChatClient::onErrorOccurred(QAbstractSocket::SocketError)
{
    emit networkError(m_socket->errorString());
}

// ---- 消息分发 ----

void ChatClient::dispatchMessage(const QJsonObject &obj)
{
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
                reason = "login failed";
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
                reason = "register failed";
            emit registerFailed(reason);
        }
        return;
    }

    // ONE_CHAT_MSG = 6
    if (msgid == 6)
    {
        const QString idText = obj.value("delivery_id").toString();
        bool validId = false;
        const quint64 deliveryId = idText.toULongLong(&validId);
        if (!idText.isEmpty() && (!validId || deliveryId == 0))
        {
            qWarning() << "invalid delivery_id in one-chat message";
            return;
        }
        if (validId && m_seenDeliveries.contains(deliveryId))
        {
            qDebug() << "DELIVERY_DUPLICATE id=" << idText;
            QJsonObject ack{{"msgid", 20}, {"delivery_id", idText}};
            sendJson(ack); // 重复投递仍需 ACK，避免 ACK 丢失后的永久重试。
            return;
        }
        int fromId = obj.value("id").toInt();
        QString name = obj.value("name").toString();
        QString msg = obj.value("msg").toString();
        QString time = obj.value("time").toString();

        emit oneChatMessageReceived(fromId, name, msg, time);
        if (validId)
        {
            m_seenDeliveries.insert(deliveryId);
            m_recentDeliveries.enqueue(deliveryId);
            while (m_recentDeliveries.size() > kRecentDeliveryLimit)
            {
                m_seenDeliveries.remove(m_recentDeliveries.dequeue());
            }
            QJsonObject ack{{"msgid", 20}, {"delivery_id", idText}};
            sendJson(ack);
        }
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

    // ADD_FRIEND_MSG_ACK = 15
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

    // CREATE_GROUP_MSG_ACK = 16
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

    // ADD_GROUP_MSG_ACK = 17
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

// ---- 请求发送 ----

void ChatClient::sendLoginRequest(int userId, const QString &password)
{
    QJsonObject obj;
    obj["msgid"] = 1;       // LOGIN_MSG
    obj["id"] = userId;
    obj["password"] = password;
    sendJson(obj);
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
    obj["msgid"] = 11;  // QUERY_ONE_CHAT_HISTORY_MSG
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
    obj["msgid"] = 13;  // QUERY_GROUP_CHAT_HISTORY_MSG
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
