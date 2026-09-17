#ifndef GATEWAY_SESSION_MANAGER_HPP
#define GATEWAY_SESSION_MANAGER_HPP

#include "session.hpp"

#include <memory>
#include <optional>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

namespace gateway
{

enum class BindResult
{
    Bound,
    DuplicateActiveSession,
    ConnectionAlreadyBound,
    InvalidConnection
};

struct BindOutcome
{
    BindResult result = BindResult::InvalidConnection;
    SessionId sessionId = 0;

    bool bound() const noexcept { return result == BindResult::Bound; }
};

// 线程安全的逻辑会话注册表。
//
// 不变量：
// 1. 同一用户最多存在一个活动会话；
// 2. SessionManager 拥有 Session，但 Session 只弱引用 TcpConnection；
// 3. 只有与当前会话匹配的连接才能解绑该用户；
// 4. 管理器锁内不执行网络、Redis 或数据库操作。
class SessionManager final
{
public:
    SessionManager() = default;
    SessionManager(const SessionManager&) = delete;
    SessionManager& operator=(const SessionManager&) = delete;

    BindOutcome bind(UserId userId,
                     const muduo::net::TcpConnectionPtr& connection);

    bool unbindUser(UserId userId,
                    const muduo::net::TcpConnectionPtr& expectedConnection);

    std::optional<UserId> unbindConnection(
        const muduo::net::TcpConnectionPtr& connection);

    std::shared_ptr<const Session> sessionForUser(UserId userId) const;

    muduo::net::TcpConnectionPtr connectionForUser(UserId userId) const;

    std::vector<muduo::net::TcpConnectionPtr> connectionsForUsers(
        const std::vector<UserId>& userIds,
        std::vector<UserId>& unavailableUserIds) const;

    std::vector<UserId> activeUserIds() const;
    std::vector<std::pair<UserId, SessionId>> activeSessions() const;
    std::shared_ptr<const Session> sessionForConnection(
        const muduo::net::TcpConnectionPtr& connection) const;
    std::size_t activeCount() const;

private:
    SessionId nextSessionId();

    mutable std::shared_mutex _mutex;
    std::unordered_map<UserId, std::shared_ptr<const Session>> _sessions;
    // 裸指针仅作为 O(1) 反向索引键；真正连接身份仍由 Session weak_ptr 校验。
    std::unordered_map<const muduo::net::TcpConnection*, UserId> _byConnection;
    SessionId _nextSessionId = 1;
};

} // namespace gateway

#endif
