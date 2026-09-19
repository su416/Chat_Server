#include <gtest/gtest.h>

#include "cluster/presence_manager.hpp"
#include "cluster/route_transport.hpp"
#include "config.hpp"

#include <chrono>
#include <thread>
#include <unistd.h>

namespace {
class ClusterTest : public ::testing::Test {
protected:
    static void SetUpTestSuite()
    {
        ASSERT_TRUE(Config::instance().load(CHAT_TEST_CONFIG));
    }
    void SetUp() override
    {
        if (!redis.connect()) GTEST_SKIP() << "Redis unavailable";
        static int next = 0;
        userId = 900000000 + (getpid() % 10000) * 100 + ++next;
        nodeA = "phase4test-" + std::to_string(getpid()) + "-A";
        nodeB = "phase4test-" + std::to_string(getpid()) + "-B";
    }
    Redis redis;
    int userId = 0;
    std::string nodeA, nodeB;
};
}

TEST_F(ClusterTest, BindLookupAndOwnerUnbind)
{
    cluster::PresenceManager a(redis, nodeA, 3);
    ASSERT_TRUE(a.registerNode());
    ASSERT_TRUE(a.bindUser(userId, 11));
    const auto found = a.lookupUser(userId);
    ASSERT_EQ(found.status, cluster::LookupStatus::Hit);
    EXPECT_EQ(found.owner.nodeId, nodeA);
    EXPECT_EQ(found.owner.sessionId, 11);
    EXPECT_TRUE(a.unbindUser(userId, 11));
    EXPECT_EQ(a.lookupUser(userId).status, cluster::LookupStatus::Miss);
    EXPECT_TRUE(a.unregisterNode());
}

TEST_F(ClusterTest, DelayedOldDisconnectCannotDeleteNewOwner)
{
    cluster::PresenceManager a(redis, nodeA, 3), b(redis, nodeB, 3);
    ASSERT_TRUE(a.registerNode());
    ASSERT_TRUE(b.registerNode());
    ASSERT_TRUE(a.bindUser(userId, 11));
    ASSERT_TRUE(b.bindUser(userId, 22));
    EXPECT_FALSE(a.unbindUser(userId, 11));
    EXPECT_EQ(a.lookupUser(userId).owner.nodeId, nodeB);
    EXPECT_FALSE(a.ownsUser(userId, 11));
    EXPECT_TRUE(b.ownsUser(userId, 22));
    EXPECT_TRUE(b.unbindUser(userId, 22));
    a.unregisterNode(); b.unregisterNode();
}

TEST_F(ClusterTest, SimultaneousBindHasSingleLastWriteOwner)
{
    cluster::PresenceManager a(redis, nodeA, 3), b(redis, nodeB, 3);
    ASSERT_TRUE(a.registerNode());
    ASSERT_TRUE(b.registerNode());
    std::thread left([&] { EXPECT_TRUE(a.bindUser(userId, 11)); });
    std::thread right([&] { EXPECT_TRUE(b.bindUser(userId, 22)); });
    left.join(); right.join();
    const auto owner = a.lookupUser(userId);
    ASSERT_EQ(owner.status, cluster::LookupStatus::Hit);
    EXPECT_TRUE(owner.owner.nodeId == nodeA || owner.owner.nodeId == nodeB);
    EXPECT_NE(a.ownsUser(userId, 11), b.ownsUser(userId, 22));
    a.unbindUser(userId, 11); b.unbindUser(userId, 22);
    a.unregisterNode(); b.unregisterNode();
}

TEST_F(ClusterTest, TtlExpiryAndNodeHeartbeat)
{
    cluster::PresenceManager a(redis, nodeA, 3);
    ASSERT_TRUE(a.registerNode());
    ASSERT_TRUE(a.bindUser(userId, 11));
    std::this_thread::sleep_for(std::chrono::seconds(2));
    EXPECT_TRUE(a.heartbeat({{userId, 11}}));
    std::this_thread::sleep_for(std::chrono::seconds(2));
    EXPECT_EQ(a.lookupUser(userId).status, cluster::LookupStatus::Hit);
    std::this_thread::sleep_for(std::chrono::seconds(2));
    EXPECT_EQ(a.lookupUser(userId).status, cluster::LookupStatus::Miss);
}

TEST_F(ClusterTest, DeadNodePresenceIsCleaned)
{
    cluster::PresenceManager a(redis, nodeA, 3);
    ASSERT_TRUE(a.registerNode());
    ASSERT_TRUE(a.bindUser(userId, 11));
    ASSERT_TRUE(a.unregisterNode());
    EXPECT_EQ(a.lookupUser(userId).status, cluster::LookupStatus::Miss);
    auto reply = redis.command({"GET", "chat:presence:user:" + std::to_string(userId)});
    ASSERT_TRUE(reply);
    EXPECT_EQ(reply->type, REDIS_REPLY_NIL);
}

TEST_F(ClusterTest, RouteIsStoredWithoutOnlineConsumer)
{
    cluster::RouteTransport transport(redis, nodeA);
    cluster::RoutedMessage route{"test-route", nodeA, nodeB,
        6, userId, userId + 1, 0, {{"msgid", 6}, {"msg", "internal"}}};
    const auto stream = cluster::RouteTransport::streamFor(nodeB);
    ASSERT_TRUE(transport.send(route));
    auto reply = redis.command({"XLEN", stream});
    ASSERT_TRUE(reply);
    ASSERT_EQ(reply->type, REDIS_REPLY_INTEGER);
    EXPECT_EQ(reply->integer, 1);
    EXPECT_EQ(transport.metrics().targetStreamPeak, 1u);
    redis.command({"DEL", stream});
}

TEST_F(ClusterTest, ConsumerAcknowledgesOnlyAcceptedRoute)
{
    cluster::RouteTransport source(redis, nodeA, "source");
    cluster::RouteTransport target(redis, nodeB, "target");
    cluster::RoutedMessage route{"test-route-" + std::to_string(userId), nodeA, nodeB,
        6, userId, userId + 1, 0, {{"msgid", 6}, {"msg", "internal"}}};
    ASSERT_TRUE(source.send(route));
    std::atomic<int> calls{0};
    ASSERT_TRUE(target.start([&](cluster::RoutedMessage received) {
        EXPECT_EQ(received.routeId, route.routeId);
        return ++calls == 1 ? cluster::RouteOutcome::Retry
                            : cluster::RouteOutcome::Accepted;
    }));
    for (int i = 0; i < 80 && target.metrics().acked == 0; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_GE(calls.load(), 2);
    EXPECT_EQ(target.metrics().acked, 1);
    EXPECT_EQ(target.metrics(true).pending, 0);
    target.stop();
    redis.command({"DEL", cluster::RouteTransport::streamFor(nodeB),
                   "chat:route:dedup:" + nodeB});
}

TEST_F(ClusterTest, RouteIdDedupIsBoundedByTtl)
{
    cluster::RouteTransport target(redis, nodeB);
    const auto routeId = "dedup-" + std::to_string(userId);
    EXPECT_EQ(target.markAccepted(routeId),
              cluster::RouteTransport::DedupResult::New);
    EXPECT_EQ(target.markAccepted(routeId),
              cluster::RouteTransport::DedupResult::Duplicate);
    auto ttl = redis.command({"TTL", "chat:route:dedup:" + nodeB});
    ASSERT_TRUE(ttl);
    EXPECT_GT(ttl->integer, 0);
    redis.command({"DEL", "chat:route:dedup:" + nodeB});
}

TEST_F(ClusterTest, UnackedPendingIsClaimedAfterConsumerRestart)
{
    const auto stream = cluster::RouteTransport::streamFor(nodeB);
    cluster::RouteTransport source(redis, nodeA, "source");
    cluster::RoutedMessage route{"restart-" + std::to_string(userId), nodeA, nodeB,
        6, userId, userId + 1, 0, {{"msgid", 6}, {"msg", "recover"}}};
    cluster::RouteTransport first(redis, nodeB, "first");
    std::atomic<int> firstCalls{0};
    ASSERT_TRUE(first.start([&](cluster::RoutedMessage received) {
        EXPECT_EQ(received.routeId, route.routeId);
        ++firstCalls;
        return cluster::RouteOutcome::Retry;
    }));
    ASSERT_TRUE(source.send(route));
    for (int i = 0; i < 30 && firstCalls == 0; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_EQ(first.metrics(true).pending, 1);
    first.stop();
    cluster::RouteTransport replacement(redis, nodeB, "replacement");
    std::atomic<int> recovered{0};
    ASSERT_TRUE(replacement.start([&](cluster::RoutedMessage received) {
        EXPECT_EQ(received.routeId, route.routeId);
        ++recovered;
        return cluster::RouteOutcome::Accepted;
    }));
    for (int i = 0; i < 80 && replacement.metrics().acked == 0; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_EQ(recovered.load(), 1);
    EXPECT_EQ(replacement.metrics().claimed, 1);
    EXPECT_EQ(replacement.metrics(true).pending, 0);
    replacement.stop();
    redis.command({"DEL", stream});
}

TEST_F(ClusterTest, InfrastructureFailureNeverAcknowledgesOrDeadLetters)
{
    const auto stream = cluster::RouteTransport::streamFor(nodeB);
    cluster::RouteTransport source(redis, nodeA, "source");
    cluster::RouteTransport target(redis, nodeB, "infra");
    cluster::RoutedMessage route{"infra-" + std::to_string(userId), nodeA, nodeB,
        6, userId, userId + 1, 0, {{"msgid", 6}, {"msg", "db-down"}}};
    std::atomic<int> calls{0};
    std::atomic<bool> recovered{false};
    ASSERT_TRUE(target.start([&](cluster::RoutedMessage) {
        ++calls;
        return recovered ? cluster::RouteOutcome::Accepted
                         : cluster::RouteOutcome::RetryInfrastructure;
    }));
    ASSERT_TRUE(source.send(route));
    for (int i = 0; i < 145 && calls.load() < 6; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_GE(calls.load(), 6); // Exceeds the normal Phase 5 retry bound.
    EXPECT_EQ(target.metrics().acked, 0u);
    EXPECT_EQ(target.metrics(true).pending, 1u);
    recovered = true;
    for (int i = 0; i < 50 && target.metrics().acked == 0; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_EQ(target.metrics().acked, 1u);
    target.stop();
    redis.command({"DEL", stream});
}

TEST_F(ClusterTest, TrimmingPreservesOlderUnackedPayload)
{
    const auto stream = cluster::RouteTransport::streamFor(nodeB);
    cluster::RouteTransport source(redis, nodeA, "source");
    cluster::RouteTransport target(redis, nodeB, "target");
    const auto base = "trim-" + std::to_string(userId);
    cluster::RoutedMessage pending{base + "-pending", nodeA, nodeB,
        6, userId, userId + 1, 0, {{"msgid", 6}, {"msg", "pending"}}};
    cluster::RoutedMessage accepted = pending;
    accepted.routeId = base + "-accepted";
    accepted.payload["msg"] = "accepted";
    ASSERT_TRUE(target.start([&](cluster::RoutedMessage route) {
        return route.routeId == accepted.routeId
            ? cluster::RouteOutcome::Accepted : cluster::RouteOutcome::Retry;
    }));
    ASSERT_TRUE(source.send(pending));
    ASSERT_TRUE(source.send(accepted));
    for (int i = 0; i < 30 && target.metrics().acked == 0; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_EQ(target.metrics(true).pending, 1);
    auto entries = redis.command({"XRANGE", stream, "-", "+"});
    ASSERT_TRUE(entries);
    ASSERT_EQ(entries->type, REDIS_REPLY_ARRAY);
    bool foundPending = false;
    for (size_t i = 0; i < entries->elements; ++i) {
        const auto* item = entries->element[i];
        if (item && item->type == REDIS_REPLY_ARRAY && item->elements == 2 &&
            item->element[1] && item->element[1]->type == REDIS_REPLY_ARRAY) {
            const auto* fields = item->element[1];
            for (size_t field = 0; field + 1 < fields->elements; field += 2)
                if (fields->element[field + 1] && fields->element[field + 1]->str &&
                    std::string(fields->element[field + 1]->str).find(pending.routeId)
                        != std::string::npos) foundPending = true;
        }
    }
    EXPECT_TRUE(foundPending);
    target.stop();
    redis.command({"DEL", stream});
}

TEST(ClusterRouteMessage, InternalMessageRoundTripAndValidation)
{
    cluster::RoutedMessage route{"route-1", "A", "B", 6, 1001, 1002, 0,
        {{"msgid", 6}, {"msg", "hello"}}};
    const auto decoded = cluster::RoutedMessage::decode(route.encode());
    ASSERT_TRUE(decoded);
    EXPECT_EQ(decoded->routeId, "route-1");
    EXPECT_EQ(decoded->payload.at("msg"), "hello");
    EXPECT_FALSE(cluster::RoutedMessage::decode("{broken"));
    route.hops = 1;
    EXPECT_TRUE(cluster::RoutedMessage::decode(route.encode()));
    route.hops = 2;
    EXPECT_FALSE(cluster::RoutedMessage::decode(route.encode()));
}
