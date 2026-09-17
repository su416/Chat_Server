#ifndef GATEWAY_SESSION_HPP
#define GATEWAY_SESSION_HPP

#include <muduo/net/TcpConnection.h>

#include <chrono>
#include <cstdint>
#include <memory>

namespace gateway
{

using SessionId = std::uint64_t;
using UserId = int;

enum class SessionState
{
    Authenticated
};

// 逻辑会话只描述已认证用户状态，不拥有 TCP 连接生命周期。
class Session final
{
public:
    Session(SessionId sessionId,
            UserId userId,
            const muduo::net::TcpConnectionPtr& connection)
        : _sessionId(sessionId),
          _userId(userId),
          _state(SessionState::Authenticated),
          _connection(connection),
          _connectionKey(connection.get()),
          _createdAt(std::chrono::steady_clock::now())
    {
    }

    SessionId id() const noexcept { return _sessionId; }
    UserId userId() const noexcept { return _userId; }
    SessionState state() const noexcept { return _state; }

    muduo::net::TcpConnectionPtr connection() const
    {
        return _connection.lock();
    }

    bool expired() const noexcept { return _connection.expired(); }
    const muduo::net::TcpConnection* connectionKey() const noexcept { return _connectionKey; }

    std::chrono::steady_clock::time_point createdAt() const noexcept
    {
        return _createdAt;
    }

private:
    const SessionId _sessionId;
    const UserId _userId;
    const SessionState _state;
    const std::weak_ptr<muduo::net::TcpConnection> _connection;
    const muduo::net::TcpConnection* const _connectionKey;
    const std::chrono::steady_clock::time_point _createdAt;
};

} // namespace gateway

#endif
