// 测试模块：RateLimiter（令牌桶限流器）
// 覆盖：首次放行、突发耗尽、禁用模式、多用户独立、清理

#include <gtest/gtest.h>
#include "rate_limiter.hpp"

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
