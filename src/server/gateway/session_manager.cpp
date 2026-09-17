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
    auto existingConnection = _byConnection.find(connection.get());
    if (existingConnection != _byConnection.end())
    {
        auto previous = _sessions.find(existingConnection->second);
        if (previous != _sessions.end() && previous->second->connection() == connection)
            return {BindResult::ConnectionAlreadyBound, previous->second->id()};
        _byConnection.erase(existingConnection);
    }
    auto it = _sessions.find(userId);
    if (it != _sessions.end())
    {
        if (it->second->connection())
        {
            return {BindResult::DuplicateActiveSession, it->second->id()};
        }

        _byConnection.erase(it->second->connectionKey());
        _sessions.erase(it);
    }

    const SessionId sessionId = nextSessionId();
    _sessions.emplace(
        userId,
        std::make_shared<const Session>(sessionId, userId, connection));
    _byConnection[connection.get()] = userId;
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

    _byConnection.erase(expectedConnection.get());
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
    auto reverse = _byConnection.find(connection.get());
    if (reverse == _byConnection.end()) return std::nullopt;
    const UserId userId = reverse->second;
    auto current = _sessions.find(userId);
    if (current == _sessions.end() || current->second->connection() != connection)
    {
        _byConnection.erase(reverse);
        return std::nullopt;
    }
    _byConnection.erase(reverse);
    _sessions.erase(current);
    return userId;
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

std::vector<std::pair<UserId, SessionId>> SessionManager::activeSessions() const
{
    std::vector<std::pair<UserId, SessionId>> result;
    std::shared_lock<std::shared_mutex> lock(_mutex);
    result.reserve(_sessions.size());
    for (const auto& [userId, session] : _sessions)
        if (!session->expired()) result.emplace_back(userId, session->id());
    return result;
}

std::shared_ptr<const Session> SessionManager::sessionForConnection(
    const muduo::net::TcpConnectionPtr& connection) const
{
    if (!connection) return nullptr;
    std::shared_lock<std::shared_mutex> lock(_mutex);
    auto reverse = _byConnection.find(connection.get());
    if (reverse == _byConnection.end()) return nullptr;
    auto current = _sessions.find(reverse->second);
    if (current != _sessions.end() && current->second->connection() == connection)
        return current->second;
    return nullptr;
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
