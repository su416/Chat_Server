#ifndef RATE_LIMITER_HPP
#define RATE_LIMITER_HPP

#include <unordered_map>
#include <mutex>
#include <cstdint>
#include <chrono>
#include <optional>
using namespace std;

// 令牌桶限流器（单例）
// 每个用户一个桶，以固定速率填充令牌，发消息消耗令牌。
// 令牌耗尽时拒绝请求，防止单用户刷屏。
class RateLimiter
{
public:
    using Clock = std::chrono::steady_clock;
    enum class Decision { Allowed, UserLimited, GlobalLimited };
    struct Metrics { std::uint64_t userRejected = 0, globalRejected = 0; };
    static RateLimiter* instance();

    // 初始化：capacity 桶容量，rate 每秒填充速率
    void init(int capacity, double rate, int globalCapacity = 0,
              double globalRate = 0.0);

    // Only ONE_CHAT/GROUP_CHAT call this; control traffic bypasses both quotas.
    Decision check(int userId, Clock::time_point now = Clock::now());
    Metrics metrics();

    // 检查是否放行，true=允许，false=限流
    bool allow(int userId);

    // 定期清理长时间未活跃的用户桶
    void cleanup(int idleSeconds);

    // 重置状态（测试用）
    void reset();

private:
    RateLimiter() = default;

    struct Bucket
    {
        double tokens;          // 当前令牌数
        int64_t lastRefill;     // 上次填充时间戳（毫秒）
    };

    int _capacity = 20;
    double _ratePerMs = 0.0;    // 每毫秒填充速率
    int _globalCapacity = 0;
    double _globalRatePerMs = 0.0;
    std::optional<Bucket> _globalBucket;
    Metrics _metrics;
    bool _initialized = false;

    unordered_map<int, Bucket> _buckets;
    mutex _mutex;
};

#endif
