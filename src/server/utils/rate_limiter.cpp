#include "rate_limiter.hpp"
#include <chrono>

RateLimiter* RateLimiter::instance()
{
    static RateLimiter limiter;
    return &limiter;
}

void RateLimiter::init(int capacity, double rate)
{
    if (_initialized) return;

    _capacity = capacity;
    _ratePerMs = rate / 1000.0;      // 转成每毫秒速率
    _initialized = true;
}

bool RateLimiter::allow(int userId)
{
    auto now = chrono::duration_cast<chrono::milliseconds>(
        chrono::steady_clock::now().time_since_epoch()).count();

    lock_guard<mutex> lg(_mutex);

    auto it = _buckets.find(userId);
    if (it == _buckets.end())
    {
        // 新用户：初始满令牌，消耗 1
        _buckets[userId] = { static_cast<double>(_capacity - 1), now };
        return true;
    }

    Bucket& b = it->second;

    // 按时间差补充令牌
    double elapsed = static_cast<double>(now - b.lastRefill);
    b.tokens += elapsed * _ratePerMs;
    if (b.tokens > _capacity)
        b.tokens = _capacity;
    b.lastRefill = now;

    // 令牌不足，拒绝
    if (b.tokens < 1.0)
        return false;

    b.tokens -= 1.0;
    return true;
}

void RateLimiter::cleanup(int idleSeconds)
{
    auto now = chrono::duration_cast<chrono::milliseconds>(
        chrono::steady_clock::now().time_since_epoch()).count();
    int64_t threshold = static_cast<int64_t>(idleSeconds) * 1000;

    lock_guard<mutex> lg(_mutex);

    auto it = _buckets.begin();
    while (it != _buckets.end())
    {
        if (now - it->second.lastRefill > threshold)
            it = _buckets.erase(it);
        else
            ++it;
    }
}
