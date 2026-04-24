#ifndef SESSIONMANAGER_H
#define SESSIONMANAGER_H

#include <QString>
#include <QHash>
#include <QJsonObject>

class SessionManager
{
public:
    SessionManager();

    void clear();

    bool isLoggedIn() const;

    void setCurrentUser(int userId, const QString &userName);
    int userId() const;
    QString userName() const;

    void rebuildFromLoginResponse(const QJsonObject &obj);

    void cacheFriend(int friendId, const QString &friendName);
    QString friendName(int friendId, const QString &fallback = "Peer") const;

    void cacheGroupMember(int groupId, int userId, const QString &userName);
    QString groupMemberName(int groupId, int userId,
                            const QString &fallback = QString()) const;

private:
    int m_currentUserId;
    QString m_currentUserName;

    QHash<int, QString> m_friendNameMap;
    QHash<int, QHash<int, QString>> m_groupMemberNameMap;
};

#endif
