// 测试模块：SessionManager（逻辑会话与 TCP 生命周期分离）

#include <gtest/gtest.h>

#include "gateway/session_manager.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

namespace
{

muduo::net::TcpConnectionPtr fakeConnection(std::uintptr_t identity)
{
    // aliasing shared_ptr 只用于身份与生命周期测试，不解引用伪连接地址。
    auto lifetime = std::make_shared<std::uintptr_t>(identity);
    return muduo::net::TcpConnectionPtr(
        lifetime,
        reinterpret_cast<muduo::net::TcpConnection*>(lifetime.get()));
}

} // namespace

TEST(SessionManagerTest, BindAndLookup)
{
    gateway::SessionManager manager;
    auto connection = fakeConnection(1);

    auto outcome = manager.bind(1001, connection);

    EXPECT_TRUE(outcome.bound());
    EXPECT_NE(outcome.sessionId, 0u);
    EXPECT_EQ(manager.connectionForUser(1001), connection);
    ASSERT_NE(manager.sessionForUser(1001), nullptr);
    EXPECT_EQ(manager.sessionForUser(1001)->userId(), 1001);
    EXPECT_EQ(manager.activeCount(), 1u);
}

TEST(SessionManagerTest, RejectsInvalidConnection)
{
    gateway::SessionManager manager;

    auto outcome = manager.bind(1001, {});

    EXPECT_EQ(outcome.result, gateway::BindResult::InvalidConnection);
    EXPECT_EQ(manager.activeCount(), 0u);
}

TEST(SessionManagerTest, RejectsDuplicateActiveLogin)
{
    gateway::SessionManager manager;
    auto first = fakeConnection(1);
    auto second = fakeConnection(2);

    auto firstOutcome = manager.bind(1001, first);
    auto duplicateOutcome = manager.bind(1001, second);

    EXPECT_TRUE(firstOutcome.bound());
    EXPECT_EQ(duplicateOutcome.result,
              gateway::BindResult::DuplicateActiveSession);
    EXPECT_EQ(duplicateOutcome.sessionId, firstOutcome.sessionId);
    EXPECT_EQ(manager.connectionForUser(1001), first);
    EXPECT_EQ(manager.activeCount(), 1u);
}

TEST(SessionManagerTest, OneConnectionCannotOwnTwoUsers)
{
    gateway::SessionManager manager;
    auto connection = fakeConnection(1);
    ASSERT_TRUE(manager.bind(1001, connection).bound());
    EXPECT_EQ(manager.bind(1002, connection).result,
              gateway::BindResult::ConnectionAlreadyBound);
    EXPECT_EQ(manager.sessionForConnection(connection)->userId(), 1001);
    EXPECT_EQ(manager.activeCount(), 1u);
}

TEST(SessionManagerTest, DisconnectBeforeLoginIsNoOp)
{
    gateway::SessionManager manager;
    auto connection = fakeConnection(1);

    EXPECT_FALSE(manager.unbindConnection(connection).has_value());
    EXPECT_EQ(manager.activeCount(), 0u);
}

TEST(SessionManagerTest, UnbindUserRequiresCurrentConnection)
{
    gateway::SessionManager manager;
    auto current = fakeConnection(1);
    auto unrelated = fakeConnection(2);
    ASSERT_TRUE(manager.bind(1001, current).bound());

    EXPECT_FALSE(manager.unbindUser(1001, unrelated));
    EXPECT_EQ(manager.connectionForUser(1001), current);
    EXPECT_TRUE(manager.unbindUser(1001, current));
    EXPECT_EQ(manager.connectionForUser(1001), nullptr);
}

TEST(SessionManagerTest, StaleDisconnectCannotRemoveReplacement)
{
    gateway::SessionManager manager;
    auto oldConnection = fakeConnection(1);
    auto newConnection = fakeConnection(2);

    ASSERT_TRUE(manager.bind(1001, oldConnection).bound());
    ASSERT_TRUE(manager.unbindUser(1001, oldConnection));
    ASSERT_TRUE(manager.bind(1001, newConnection).bound());

    EXPECT_FALSE(manager.unbindConnection(oldConnection).has_value());
    EXPECT_EQ(manager.connectionForUser(1001), newConnection);
}

TEST(SessionManagerTest, ExpiredConnectionCanBeRebound)
{
    gateway::SessionManager manager;
    auto oldConnection = fakeConnection(1);
    auto firstOutcome = manager.bind(1001, oldConnection);
    ASSERT_TRUE(firstOutcome.bound());

    oldConnection.reset();
    auto newConnection = fakeConnection(2);
    auto secondOutcome = manager.bind(1001, newConnection);

    EXPECT_TRUE(secondOutcome.bound());
    EXPECT_NE(secondOutcome.sessionId, firstOutcome.sessionId);
    EXPECT_EQ(manager.connectionForUser(1001), newConnection);
    EXPECT_EQ(manager.activeCount(), 1u);
}

TEST(SessionManagerTest, BatchLookupSeparatesUnavailableUsers)
{
    gateway::SessionManager manager;
    auto first = fakeConnection(1);
    auto third = fakeConnection(3);
    ASSERT_TRUE(manager.bind(1, first).bound());
    ASSERT_TRUE(manager.bind(3, third).bound());

    std::vector<int> unavailable;
    auto connections = manager.connectionsForUsers({1, 2, 3}, unavailable);

    ASSERT_EQ(connections.size(), 2u);
    EXPECT_EQ(connections[0], first);
    EXPECT_EQ(connections[1], third);
    EXPECT_EQ(unavailable, std::vector<int>({2}));
}

TEST(SessionManagerTest, ConcurrentDuplicateBindHasSingleWinner)
{
    gateway::SessionManager manager;
    constexpr int threadCount = 8;
    std::vector<muduo::net::TcpConnectionPtr> connections;
    connections.reserve(threadCount);
    for (int i = 0; i < threadCount; ++i)
    {
        connections.push_back(fakeConnection(static_cast<std::uintptr_t>(i + 1)));
    }

    std::atomic<int> winners{0};
    std::vector<std::thread> threads;
    threads.reserve(threadCount);
    for (int i = 0; i < threadCount; ++i)
    {
        threads.emplace_back([&manager, &connections, &winners, i]() {
            if (manager.bind(1001, connections[i]).bound())
            {
                winners.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    for (auto& thread : threads)
    {
        thread.join();
    }

    EXPECT_EQ(winners.load(std::memory_order_relaxed), 1);
    EXPECT_EQ(manager.activeCount(), 1u);
}
