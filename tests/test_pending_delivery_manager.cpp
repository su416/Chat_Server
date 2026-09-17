#include <gtest/gtest.h>

#include "gateway/pending_delivery_manager.hpp"
#include "public.hpp"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

namespace
{
using gateway::PendingDeliveryManager;
using Clock = PendingDeliveryManager::Clock;

muduo::net::TcpConnectionPtr fakeConnection(std::uintptr_t identity)
{
    auto lifetime = std::make_shared<std::uintptr_t>(identity);
    return muduo::net::TcpConnectionPtr(
        lifetime, reinterpret_cast<muduo::net::TcpConnection*>(lifetime.get()));
}

std::shared_ptr<const gateway::Session> makeSession(
    gateway::SessionId id, int user,
    const muduo::net::TcpConnectionPtr& connection)
{
    return std::make_shared<const gateway::Session>(id, user, connection);
}

gateway::MessageEnvelope message(std::uint64_t id)
{
    return {ONE_CHAT_MSG, {{"msgid", ONE_CHAT_MSG}, {"msg", "hello"}},
            protocol::WireProtocol::Json, id};
}

gateway::DeliveryPolicy fastPolicy()
{
    return {std::chrono::milliseconds(100), 2, 100};
}
} // namespace

TEST(PendingDeliveryTest, NormalAckAndDuplicateAck)
{
    PendingDeliveryManager manager(fastPolicy());
    auto connection = fakeConnection(1);
    auto session = makeSession(1, 1001, connection);
    const auto id = manager.nextDeliveryId();
    ASSERT_TRUE(manager.track(session, message(id)));
    EXPECT_EQ(manager.ack(id, connection), gateway::AckResult::Acked);
    EXPECT_EQ(manager.ack(id, connection), gateway::AckResult::Unknown);
    EXPECT_EQ(manager.ack(99999999, connection), gateway::AckResult::Unknown);
    const auto metrics = manager.metrics();
    EXPECT_EQ(metrics.sent, 1u);
    EXPECT_EQ(metrics.acked, 1u);
    EXPECT_EQ(metrics.pending, 0u);
}

TEST(PendingDeliveryTest, TimeoutRetryKeepsIdAndLateAckStopsRetry)
{
    PendingDeliveryManager manager(fastPolicy());
    auto connection = fakeConnection(1);
    auto session = makeSession(1, 1001, connection);
    const auto id = manager.nextDeliveryId();
    const auto start = Clock::now();
    ASSERT_TRUE(manager.track(session, message(id), start));
    EXPECT_TRUE(manager.scanExpired(start + std::chrono::milliseconds(99)).empty());
    const auto retries = manager.scanExpired(start + std::chrono::milliseconds(100));
    ASSERT_EQ(retries.size(), 1u);
    EXPECT_EQ(retries[0].envelope.deliveryId(), id);
    EXPECT_EQ(retries[0].retryCount, 1u);
    EXPECT_EQ(manager.ack(id, connection), gateway::AckResult::Acked);
    EXPECT_TRUE(manager.scanExpired(start + std::chrono::seconds(1)).empty());
    EXPECT_EQ(manager.metrics().retried, 1u);
}

TEST(PendingDeliveryTest, MaxRetryRemovesPending)
{
    PendingDeliveryManager manager(fastPolicy());
    auto connection = fakeConnection(1);
    auto session = makeSession(1, 1001, connection);
    const auto id = manager.nextDeliveryId();
    const auto start = Clock::now();
    ASSERT_TRUE(manager.track(session, message(id), start));
    EXPECT_EQ(manager.scanExpired(start + std::chrono::milliseconds(100)).size(), 1u);
    EXPECT_EQ(manager.scanExpired(start + std::chrono::milliseconds(200)).size(), 1u);
    EXPECT_TRUE(manager.scanExpired(start + std::chrono::milliseconds(300)).empty());
    const auto metrics = manager.metrics();
    EXPECT_EQ(metrics.retried, 2u);
    EXPECT_EQ(metrics.timedOut, 3u);
    EXPECT_EQ(metrics.failed, 1u);
    EXPECT_EQ(metrics.pending, 0u);
    EXPECT_EQ(manager.ack(id, connection), gateway::AckResult::Unknown);
}

TEST(PendingDeliveryTest, WrongSessionCannotAckAndOutOfOrderAckWorks)
{
    PendingDeliveryManager manager(fastPolicy());
    auto owner = fakeConnection(1);
    auto impostor = fakeConnection(2);
    auto session = makeSession(1, 1001, owner);
    const auto first = manager.nextDeliveryId();
    const auto second = manager.nextDeliveryId();
    ASSERT_TRUE(manager.track(session, message(first)));
    ASSERT_TRUE(manager.track(session, message(second)));
    EXPECT_EQ(manager.ack(first, impostor), gateway::AckResult::WrongSession);
    EXPECT_EQ(manager.ack(second, owner), gateway::AckResult::Acked);
    EXPECT_EQ(manager.ack(first, owner), gateway::AckResult::Acked);
    EXPECT_EQ(manager.metrics().pendingPeak, 2u);
}

TEST(PendingDeliveryTest, DisconnectAndExpiredSessionDoNotRetry)
{
    PendingDeliveryManager manager(fastPolicy());
    auto connection = fakeConnection(1);
    auto session = makeSession(1, 1001, connection);
    const auto first = manager.nextDeliveryId();
    ASSERT_TRUE(manager.track(session, message(first)));
    EXPECT_EQ(manager.cancelConnection(connection), 1u);
    EXPECT_TRUE(manager.scanExpired(Clock::now() + std::chrono::seconds(1)).empty());

    const auto second = manager.nextDeliveryId();
    ASSERT_TRUE(manager.track(session, message(second)));
    session.reset();
    EXPECT_TRUE(manager.scanExpired(Clock::now() + std::chrono::seconds(1)).empty());
    EXPECT_EQ(manager.metrics().pending, 0u);
    EXPECT_EQ(manager.metrics().failed, 2u);
}

TEST(PendingDeliveryTest, CapacityIsBounded)
{
    auto policy = fastPolicy();
    policy.maxPending = 1;
    PendingDeliveryManager manager(policy);
    auto connection = fakeConnection(1);
    auto session = makeSession(1, 1001, connection);
    ASSERT_TRUE(manager.track(session, message(manager.nextDeliveryId())));
    EXPECT_FALSE(manager.track(session, message(manager.nextDeliveryId())));
    EXPECT_EQ(manager.metrics().pendingPeak, 1u);
}

TEST(PendingDeliveryTest, PayloadBytesAreBoundedAndReleasedAfterAck)
{
    auto policy = fastPolicy();
    policy.maxPendingBytes = message(1).payload().dump().size() + 25;
    PendingDeliveryManager manager(policy);
    auto connection = fakeConnection(1);
    auto session = makeSession(1, 1001, connection);
    const auto first = manager.nextDeliveryId();
    const auto second = manager.nextDeliveryId();
    ASSERT_TRUE(manager.track(session, message(first)));
    EXPECT_FALSE(manager.track(session, message(second)));
    EXPECT_GT(manager.metrics().pendingBytes, 0u);
    EXPECT_EQ(manager.ack(first, connection), gateway::AckResult::Acked);
    EXPECT_EQ(manager.metrics().pendingBytes, 0u);
    EXPECT_TRUE(manager.track(session, message(second)));
}

TEST(PendingDeliveryTest, IdGenerationIsConcurrentAndUnique)
{
    PendingDeliveryManager manager;
    std::vector<std::thread> workers;
    std::vector<std::vector<std::uint64_t>> results(8);
    for (std::size_t i = 0; i < results.size(); ++i)
    {
        workers.emplace_back([&manager, &results, i]() {
            for (int n = 0; n < 1000; ++n)
                results[i].push_back(manager.nextDeliveryId());
        });
    }
    for (auto& worker : workers) worker.join();
    std::vector<std::uint64_t> all;
    for (const auto& result : results)
        all.insert(all.end(), result.begin(), result.end());
    std::sort(all.begin(), all.end());
    EXPECT_EQ(std::unique(all.begin(), all.end()), all.end());
    EXPECT_EQ(all.size(), 8000u);
}
