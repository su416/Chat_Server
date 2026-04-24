#ifndef CHATCLIENT_H
#define CHATCLIENT_H

#include <QObject>
#include <QTcpSocket>
#include <QJsonObject>
#include <QJsonArray>

class ChatClient : public QObject
{
    Q_OBJECT

public:
    explicit ChatClient(QObject *parent = nullptr);

    void connectToServer(const QString &host, quint16 port);
    void sendLoginRequest(int userId, const QString &password);
    void sendRegisterRequest(const QString &name, const QString &password);
    void sendOneChatRequest(int userId,
                            int toId,
                            const QString &name,
                            const QString &msg,
                            const QString &time);
    void sendQueryOneChatHistoryRequest(int userId, int limit);
    void sendAddFriendRequest(int userId, int friendId);
    void sendCreateGroupRequest(int userId, const QString &groupName, const QString &groupDesc);
    void sendJoinGroupRequest(int userId, int groupId);
    void sendGroupChatRequest(int userId,
                              int groupId,
                              const QString &name,
                              const QString &msg,
                              const QString &time);
                                  void sendQueryGroupChatHistoryRequest(int groupId, int limit);
void sendLogoutRequest(int userId);

void disconnectFromServer();

signals:
    void connected();
    void disconnected();
    void loginSucceeded(const QJsonObject &response);
    void loginFailed(const QString &reason);
    void registerSucceeded(int userId);
    void registerFailed(const QString &reason);
    void oneChatMessageReceived(int fromId,
                                const QString &name,
                                const QString &msg,
                                const QString &time);
    void oneChatHistoryReceived(const QJsonArray &history);
    void networkError(const QString &reason);

    void addFriendSucceeded(int friendId, const QString &friendName, const QString &state);
    void addFriendFailed(const QString &reason);

    void createGroupSucceeded(int groupId, const QString &groupName, const QString &groupDesc);
    void createGroupFailed(const QString &reason);
    void joinGroupSucceeded(int groupId, const QString &groupName, const QString &groupDesc);
    void joinGroupFailed(const QString &reason);
    void groupChatMessageReceived(int fromId,
                                  int groupId,
                                  const QString &name,
                                  const QString &msg,
                                  const QString &time);

                                      void groupChatHistoryReceived(int groupId, const QJsonArray &history);


private slots:
    void onConnected();
    void onDisconnected();
    void onReadyRead();
    void onErrorOccurred(QAbstractSocket::SocketError socketError);

private:
    void sendJson(const QJsonObject &obj);

private:
    QTcpSocket *m_socket;
};

#endif

