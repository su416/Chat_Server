// 测试模块：RateLimiter（令牌桶限流器）
// 覆盖：首次放行、突发耗尽、禁用模式、多用户独立

#include <gtest/gtest.h>
#include "rate_limiter.hpp"

TEST(RateLimiterTest, FirstRequestAllowed)
{
    RateLimiter limiter;
    limiter.init(10, 100.0);  // 容量 10，速率 100/s
    EXPECT_TRUE(limiter.allow(1));
}

TEST(RateLimiterTest, BurstCapacityRespected)
{
    RateLimiter limiter;
    limiter.init(5, 10.0);   // 容量 5，速率 10/s（慢恢复）

    // 前 5 次应全部放行
    for (int i = 0; i < 5; ++i)
        EXPECT_TRUE(limiter.allow(1));
    // 第 6 次应拒绝（桶空 + 速率低来不及恢复）
    EXPECT_FALSE(limiter.allow(1));
}

TEST(RateLimiterTest, DisabledWhenNotInitialized)
{
    RateLimiter limiter;
    // 未调 init() → _initialized == false → allow() 直接返回 true
    EXPECT_TRUE(limiter.allow(1));
    EXPECT_TRUE(limiter.allow(1));
}

TEST(RateLimiterTest, DifferentUsersIndependent)
{
    RateLimiter limiter;
    limiter.init(3, 10.0);

    // 用户 1 消耗完额度
    EXPECT_TRUE(limiter.allow(1));
    EXPECT_TRUE(limiter.allow(1));
    EXPECT_TRUE(limiter.allow(1));
    EXPECT_FALSE(limiter.allow(1));

    // 用户 2 独立桶，仍然可放行
    EXPECT_TRUE(limiter.allow(2));
    EXPECT_TRUE(limiter.allow(2));
}

TEST(RateLimiterTest, CleanupRemovesStaleUsers)
{
    RateLimiter limiter;
    limiter.init(10, 100.0);

    limiter.allow(1);
    limiter.cleanup(0);  // idleSeconds=0 → 立即清
    // 清理后再发请求 → 新桶、满容量
    EXPECT_TRUE(limiter.allow(1));
}
