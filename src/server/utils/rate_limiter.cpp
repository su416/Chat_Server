#include "rate_limiter.hpp"
#include <chrono>
#include <algorithm>
#include <cmath>
#include <stdexcept>

RateLimiter* RateLimiter::instance()
{
    static RateLimiter limiter;
    return &limiter;
}

void RateLimiter::init(int capacity, double rate, int globalCapacity, double globalRate)
{
    lock_guard<mutex> lg(_mutex);
    if (_initialized) return;
    if (capacity <= 0 || !std::isfinite(rate) || rate <= 0 ||
        globalCapacity < 0 || !std::isfinite(globalRate) || globalRate < 0 ||
        ((globalCapacity == 0) != (globalRate == 0)))
        throw std::invalid_argument("invalid token bucket configuration");

    _capacity = capacity;
    _ratePerMs = rate / 1000.0;      // 转成每毫秒速率
    _globalCapacity = globalCapacity;
    _globalRatePerMs = globalRate / 1000.0;
    _initialized = true;
}

bool RateLimiter::allow(int userId)
{
    return check(userId) == Decision::Allowed;
}

RateLimiter::Decision RateLimiter::check(int userId, Clock::time_point timestamp)
{
    const auto now = chrono::duration_cast<chrono::milliseconds>(
        timestamp.time_since_epoch()).count();
    lock_guard<mutex> lg(_mutex);
    if (!_initialized) return Decision::Allowed;

    auto refill = [now](Bucket& bucket, int capacity, double rate) {
        if (now > bucket.lastRefill) {
            bucket.tokens = std::min(static_cast<double>(capacity),
                bucket.tokens + static_cast<double>(now - bucket.lastRefill) * rate);
            bucket.lastRefill = now;
        }
    };
    auto& user = _buckets.try_emplace(userId,
        Bucket{static_cast<double>(_capacity), now}).first->second;
    refill(user, _capacity, _ratePerMs);
    if (user.tokens < 1.0) {
        ++_metrics.userRejected;
        return Decision::UserLimited;
    }
    if (_globalCapacity > 0) {
        if (!_globalBucket)
            _globalBucket = Bucket{static_cast<double>(_globalCapacity), now};
        refill(*_globalBucket, _globalCapacity, _globalRatePerMs);
        if (_globalBucket->tokens < 1.0) {
            ++_metrics.globalRejected;
            return Decision::GlobalLimited;
        }
        _globalBucket->tokens -= 1.0;
    }
    // Atomic admission: a global rejection does not consume user tokens.
    user.tokens -= 1.0;
    return Decision::Allowed;
}

RateLimiter::Metrics RateLimiter::metrics()
{
    lock_guard<mutex> lg(_mutex);
    return _metrics;
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

void RateLimiter::reset()
{
    lock_guard<mutex> lg(_mutex);
    _buckets.clear();
    _globalBucket.reset();
    _metrics = {};
    _initialized = false;
}
