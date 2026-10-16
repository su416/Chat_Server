// 测试模块：RateLimiter（令牌桶限流器）
// 覆盖：首次放行、突发耗尽、禁用模式、多用户独立、清理

#include <gtest/gtest.h>
#include "rate_limiter.hpp"
#include "config.hpp"
#include "json.hpp"
#include <atomic>
#include <fstream>
#include <limits>
#include <thread>
#include <vector>
#include <unistd.h>

class RateLimiterTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        RateLimiter::instance()->reset();
    }
};

TEST_F(RateLimiterTest, FirstRequestAllowed)
{
    RateLimiter::instance()->init(10, 100.0);
    EXPECT_TRUE(RateLimiter::instance()->allow(1));
}

TEST_F(RateLimiterTest, BurstCapacityRespected)
{
    RateLimiter::instance()->init(5, 10.0);

    for (int i = 0; i < 5; ++i)
        EXPECT_TRUE(RateLimiter::instance()->allow(1));
    EXPECT_FALSE(RateLimiter::instance()->allow(1));
}

TEST_F(RateLimiterTest, DisabledWhenNotInitialized)
{
    // reset() 后未调 init → allow() 直接返回 true
    EXPECT_TRUE(RateLimiter::instance()->allow(1));
    EXPECT_TRUE(RateLimiter::instance()->allow(1));
}

TEST_F(RateLimiterTest, DifferentUsersIndependent)
{
    RateLimiter::instance()->init(3, 10.0);

    EXPECT_TRUE(RateLimiter::instance()->allow(1));
    EXPECT_TRUE(RateLimiter::instance()->allow(1));
    EXPECT_TRUE(RateLimiter::instance()->allow(1));
    EXPECT_FALSE(RateLimiter::instance()->allow(1));

    // 用户 2 独立桶
    EXPECT_TRUE(RateLimiter::instance()->allow(2));
}

TEST_F(RateLimiterTest, CleanupRemovesStaleUsers)
{
    RateLimiter::instance()->init(10, 100.0);
    RateLimiter::instance()->allow(1);

    RateLimiter::instance()->cleanup(0);
    // 清理后再发 → 新桶满容量
    EXPECT_TRUE(RateLimiter::instance()->allow(1));
}

TEST_F(RateLimiterTest, RefillWithoutSleeping)
{
    auto* limiter = RateLimiter::instance();
    limiter->init(1, 2.0, 1, 2.0);
    const auto now = RateLimiter::Clock::now();
    EXPECT_EQ(limiter->check(1, now), RateLimiter::Decision::Allowed);
    EXPECT_EQ(limiter->check(1, now), RateLimiter::Decision::UserLimited);
    EXPECT_EQ(limiter->check(2, now), RateLimiter::Decision::GlobalLimited);
    EXPECT_EQ(limiter->check(1, now + std::chrono::milliseconds(499)),
              RateLimiter::Decision::UserLimited);
    EXPECT_EQ(limiter->check(1, now + std::chrono::milliseconds(500)),
              RateLimiter::Decision::Allowed);
    EXPECT_EQ(limiter->check(2, now + std::chrono::seconds(1)),
              RateLimiter::Decision::Allowed);
}

TEST_F(RateLimiterTest, IndependentUsersShareGlobalQuota)
{
    auto* limiter = RateLimiter::instance();
    limiter->init(10, 1, 2, 1);
    const auto now = RateLimiter::Clock::now();
    EXPECT_EQ(limiter->check(1, now), RateLimiter::Decision::Allowed);
    EXPECT_EQ(limiter->check(2, now), RateLimiter::Decision::Allowed);
    EXPECT_EQ(limiter->check(3, now), RateLimiter::Decision::GlobalLimited);
    EXPECT_EQ(limiter->metrics().userRejected, 0u);
    EXPECT_EQ(limiter->metrics().globalRejected, 1u);
}

TEST_F(RateLimiterTest, RejectedUserDoesNotConsumeGlobalTokens)
{
    auto* limiter = RateLimiter::instance();
    limiter->init(1, 1, 2, 1);
    const auto now = RateLimiter::Clock::now();
    EXPECT_EQ(limiter->check(1, now), RateLimiter::Decision::Allowed);
    EXPECT_EQ(limiter->check(1, now), RateLimiter::Decision::UserLimited);
    EXPECT_EQ(limiter->check(2, now), RateLimiter::Decision::Allowed);
}

TEST_F(RateLimiterTest, GlobalRejectionDoesNotConsumeUserTokens)
{
    auto* limiter = RateLimiter::instance();
    limiter->init(2, 0.001, 1, 1);
    const auto now = RateLimiter::Clock::now();
    EXPECT_EQ(limiter->check(1, now), RateLimiter::Decision::Allowed);
    EXPECT_EQ(limiter->check(2, now), RateLimiter::Decision::GlobalLimited);
    EXPECT_EQ(limiter->check(2, now), RateLimiter::Decision::GlobalLimited);
    EXPECT_EQ(limiter->check(2, now + std::chrono::seconds(1)), RateLimiter::Decision::Allowed);
    EXPECT_EQ(limiter->check(2, now + std::chrono::seconds(2)), RateLimiter::Decision::Allowed);
}

TEST_F(RateLimiterTest, ConcurrentGlobalCapacityIsExact)
{
    auto* limiter = RateLimiter::instance();
    limiter->init(100, 1, 37, 1);
    const auto now = RateLimiter::Clock::now();
    std::atomic<int> accepted{0};
    std::vector<std::thread> workers;
    for (int user = 0; user < 8; ++user)
        workers.emplace_back([&, user] {
            for (int i = 0; i < 100; ++i)
                if (limiter->check(user, now) == RateLimiter::Decision::Allowed)
                    ++accepted;
        });
    for (auto& worker : workers) worker.join();
    EXPECT_EQ(accepted.load(), 37);
    EXPECT_EQ(limiter->metrics().globalRejected, 800u - 37);
}

TEST_F(RateLimiterTest, InvalidParametersFailExplicitly)
{
    auto* limiter = RateLimiter::instance();
    EXPECT_THROW(limiter->init(0, 1), std::invalid_argument);
    EXPECT_THROW(limiter->init(1, -1), std::invalid_argument);
    EXPECT_THROW(limiter->init(1, std::numeric_limits<double>::infinity()), std::invalid_argument);
    EXPECT_THROW(limiter->init(1, 1, 1, 0), std::invalid_argument);
}

TEST_F(RateLimiterTest, ConfigCompatibilityAndValidation)
{
    nlohmann::json document;
    std::ifstream(CHAT_TEST_CONFIG) >> document;
    char path[] = "/tmp/chat-rate-config-XXXXXX";
    const int fd = mkstemp(path);
    ASSERT_GE(fd, 0);
    close(fd);
    auto load = [&] {
        { std::ofstream output(path); output << document.dump(); }
        return Config::instance().load(path);
    };
    document["rate_limit"] = {{"enabled", true}, {"capacity", 7}, {"rate_per_sec", 3.0}};
    EXPECT_TRUE(load());
    EXPECT_EQ(Config::instance().getRateLimitCapacity(), 7);
    document["rate_limit"]["user_capacity"] = 9;
    document["rate_limit"]["global_capacity"] = 15;
    EXPECT_TRUE(load());
    EXPECT_EQ(Config::instance().getRateLimitCapacity(), 9);
    EXPECT_EQ(Config::instance().getGlobalRateLimitCapacity(), 15);
    document["rate_limit"]["global_rate_per_sec"] = -1;
    EXPECT_FALSE(load());
    document.erase("rate_limit");
    EXPECT_TRUE(load());
    EXPECT_FALSE(Config::instance().isRateLimitEnabled());
    unlink(path);
}
