#include "service/sessionmanager.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>

namespace
{
QJsonObject parseJsonObjectFromValue(const QJsonValue &value)
{
    if (value.isObject())
    {
        return value.toObject();
    }

    if (value.isString())
    {
        QJsonParseError err;
        QJsonDocument doc = QJsonDocument::fromJson(value.toString().toUtf8(), &err);
        if (err.error == QJsonParseError::NoError && doc.isObject())
        {
            return doc.object();
        }
    }

    return QJsonObject();
}
}

SessionManager::SessionManager()
    : m_currentUserId(-1)
{
}

void SessionManager::clear()
{
    m_currentUserId = -1;
    m_currentUserName.clear();
    m_friendNameMap.clear();
    m_groupMemberNameMap.clear();
}

bool SessionManager::isLoggedIn() const
{
    return m_currentUserId > 0;
}

void SessionManager::setCurrentUser(int userId, const QString &userName)
{
    m_currentUserId = userId;
    m_currentUserName = userName;
}

int SessionManager::userId() const
{
    return m_currentUserId;
}

QString SessionManager::userName() const
{
    return m_currentUserName;
}

void SessionManager::rebuildFromLoginResponse(const QJsonObject &obj)
{
    m_friendNameMap.clear();
    m_groupMemberNameMap.clear();

    if (obj.contains("friends") && obj.value("friends").isArray())
    {
        QJsonArray arr = obj.value("friends").toArray();
        for (const auto &item : arr)
        {
            QJsonObject friendObj = parseJsonObjectFromValue(item);
            if (friendObj.isEmpty())
            {
                continue;
            }

            int id = friendObj.value("id").toInt();
            QString name = friendObj.value("name").toString();

            if (id > 0 && !name.isEmpty())
            {
                m_friendNameMap[id] = name;
            }
        }
    }

    if (obj.contains("groups") && obj.value("groups").isArray())
    {
        QJsonArray arr = obj.value("groups").toArray();
        for (const auto &item : arr)
        {
            QJsonObject groupObj = parseJsonObjectFromValue(item);
            if (groupObj.isEmpty())
            {
                continue;
            }

            int groupId = groupObj.contains("id")
                              ? groupObj.value("id").toInt()
                              : groupObj.value("groupid").toInt();

            if (groupId <= 0)
            {
                continue;
            }

            QHash<int, QString> memberMap;

            if (groupObj.contains("users") && groupObj.value("users").isArray())
            {
                QJsonArray users = groupObj.value("users").toArray();
                for (const auto &userItem : users)
                {
                    QJsonObject userObj = parseJsonObjectFromValue(userItem);
                    if (userObj.isEmpty())
                    {
                        continue;
                    }

                    int uid = userObj.value("id").toInt();
                    QString uname = userObj.value("name").toString();

                    if (uid > 0 && !uname.isEmpty())
                    {
                        memberMap[uid] = uname;
                    }
                }
            }

            m_groupMemberNameMap[groupId] = memberMap;
        }
    }
}

void SessionManager::cacheFriend(int friendId, const QString &friendName)
{
    if (friendId > 0 && !friendName.isEmpty())
    {
        m_friendNameMap[friendId] = friendName;
    }
}

QString SessionManager::friendName(int friendId, const QString &fallback) const
{
    return m_friendNameMap.value(friendId, fallback);
}

void SessionManager::cacheGroupMember(int groupId, int userId, const QString &userName)
{
    if (groupId > 0 && userId > 0 && !userName.isEmpty())
    {
        m_groupMemberNameMap[groupId][userId] = userName;
    }
}

QString SessionManager::groupMemberName(int groupId, int userId, const QString &fallback) const
{
    if (m_groupMemberNameMap.contains(groupId))
    {
        const auto &memberMap = m_groupMemberNameMap[groupId];
        if (memberMap.contains(userId))
        {
            return memberMap[userId];
        }
    }

    if (!fallback.isEmpty())
    {
        return fallback;
    }

    return QString("User%1").arg(userId);
}
