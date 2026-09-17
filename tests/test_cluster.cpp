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

TEST_F(ClusterTest, UnknownNodeHasNoSubscriber)
{
    cluster::RouteTransport transport(redis, nodeA);
    cluster::RoutedMessage route{"test-route", nodeA, "nonexistent-target-node",
        6, userId, userId + 1, 0, {{"msgid", 6}, {"msg", "internal"}}};
    EXPECT_FALSE(transport.send(route));
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
    route.hops = 2;
    EXPECT_FALSE(cluster::RoutedMessage::decode(route.encode()));
}
