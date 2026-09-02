#include "gateway/session_manager.hpp"

#include <limits>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace gateway
{

SessionId SessionManager::nextSessionId()
{
    // 该方法仅在 bind() 持有独占锁时调用。
    if (_nextSessionId == 0)
    {
        throw std::overflow_error("session id space exhausted");
    }

    const SessionId id = _nextSessionId;
    if (_nextSessionId == std::numeric_limits<SessionId>::max())
    {
        _nextSessionId = 0;
    }
    else
    {
        ++_nextSessionId;
    }
    return id;
}

BindOutcome SessionManager::bind(
    UserId userId,
    const muduo::net::TcpConnectionPtr& connection)
{
    if (!connection)
    {
        return {BindResult::InvalidConnection, 0};
    }

    std::unique_lock<std::shared_mutex> lock(_mutex);
    auto it = _sessions.find(userId);
    if (it != _sessions.end())
    {
        if (it->second->connection())
        {
            return {BindResult::DuplicateActiveSession, it->second->id()};
        }

        _sessions.erase(it);
    }

    const SessionId sessionId = nextSessionId();
    _sessions.emplace(
        userId,
        std::make_shared<const Session>(sessionId, userId, connection));
    return {BindResult::Bound, sessionId};
}

bool SessionManager::unbindUser(
    UserId userId,
    const muduo::net::TcpConnectionPtr& expectedConnection)
{
    if (!expectedConnection)
    {
        return false;
    }

    std::unique_lock<std::shared_mutex> lock(_mutex);
    auto it = _sessions.find(userId);
    if (it == _sessions.end())
    {
        return false;
    }

    auto currentConnection = it->second->connection();
    if (!currentConnection || currentConnection != expectedConnection)
    {
        return false;
    }

    _sessions.erase(it);
    return true;
}

std::optional<UserId> SessionManager::unbindConnection(
    const muduo::net::TcpConnectionPtr& connection)
{
    if (!connection)
    {
        return std::nullopt;
    }

    std::unique_lock<std::shared_mutex> lock(_mutex);
    for (auto it = _sessions.begin(); it != _sessions.end(); ++it)
    {
        auto currentConnection = it->second->connection();
        if (currentConnection && currentConnection == connection)
        {
            const UserId userId = it->first;
            _sessions.erase(it);
            return userId;
        }
    }

    return std::nullopt;
}

std::shared_ptr<const Session> SessionManager::sessionForUser(UserId userId) const
{
    std::shared_lock<std::shared_mutex> lock(_mutex);
    auto it = _sessions.find(userId);
    if (it == _sessions.end() || it->second->expired())
    {
        return nullptr;
    }
    return it->second;
}

muduo::net::TcpConnectionPtr SessionManager::connectionForUser(UserId userId) const
{
    auto session = sessionForUser(userId);
    return session ? session->connection() : nullptr;
}

std::vector<muduo::net::TcpConnectionPtr> SessionManager::connectionsForUsers(
    const std::vector<UserId>& userIds,
    std::vector<UserId>& unavailableUserIds) const
{
    std::vector<muduo::net::TcpConnectionPtr> connections;
    connections.reserve(userIds.size());
    unavailableUserIds.clear();
    unavailableUserIds.reserve(userIds.size());

    std::shared_lock<std::shared_mutex> lock(_mutex);
    for (UserId userId : userIds)
    {
        auto it = _sessions.find(userId);
        if (it != _sessions.end())
        {
            auto connection = it->second->connection();
            if (connection)
            {
                connections.push_back(std::move(connection));
                continue;
            }
        }
        unavailableUserIds.push_back(userId);
    }

    return connections;
}

std::vector<UserId> SessionManager::activeUserIds() const
{
    std::vector<UserId> userIds;
    std::shared_lock<std::shared_mutex> lock(_mutex);
    userIds.reserve(_sessions.size());
    for (const auto& entry : _sessions)
    {
        if (!entry.second->expired())
        {
            userIds.push_back(entry.first);
        }
    }
    return userIds;
}

std::size_t SessionManager::activeCount() const
{
    std::size_t count = 0;
    std::shared_lock<std::shared_mutex> lock(_mutex);
    for (const auto& entry : _sessions)
    {
        if (!entry.second->expired())
        {
            ++count;
        }
    }
    return count;
}

} // namespace gateway
