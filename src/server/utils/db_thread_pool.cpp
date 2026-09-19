#include "db_thread_pool.hpp"
#include <muduo/base/Logging.h>
#include <chrono>
#include <cstdlib>
#include <algorithm>
#include <stdexcept>

DBThreadPool* DBThreadPool::instance()
{
    static DBThreadPool pool;
    return &pool;
}

void DBThreadPool::init(int threadCount, std::size_t maxQueued,
                        std::size_t reservedHigh)
{
    if (threadCount < 1 || maxQueued < 2 || reservedHigh >= maxQueued)
        throw std::invalid_argument("invalid DB worker configuration");
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_running) return;
        _capacity = maxQueued;
        _reservedHigh = reservedHigh;
        _running = true;
    }
    for (int i = 0; i < threadCount; ++i)
    {
        _workers.emplace_back(&DBThreadPool::workerLoop, this);
    }
    LOG_INFO << "DBThreadPool: threads=" << threadCount
             << " queue_capacity=" << maxQueued << " reserved_high=" << reservedHigh;
}

DBThreadPool::SubmitResult DBThreadPool::trySubmit(
    std::function<void()> task, Priority priority)
{
    if (!task) return SubmitResult::Stopped;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (!_running) return SubmitResult::Stopped;
        const auto depth = _tasks[0].size() + _tasks[1].size() + _tasks[2].size();
        const auto limit = priority == Priority::High ? _capacity :
            priority == Priority::Normal ? _capacity - _reservedHigh :
            _capacity / 2;
        if (depth >= limit) { ++_rejected; return SubmitResult::QueueFull; }
        _tasks[static_cast<int>(priority)].emplace_back(std::move(task));
        _peak = std::max(_peak, depth + 1);
    }
    _cv.notify_one();
    return SubmitResult::Accepted;
}

DBThreadPool::Metrics DBThreadPool::metrics() const {
    std::lock_guard<std::mutex> lock(_mutex);
    return {_tasks[0].size() + _tasks[1].size() + _tasks[2].size(),
            _capacity, _peak, _active, _rejected};
}

void DBThreadPool::workerLoop()
{
    while (true)
    {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(_mutex);
            _cv.wait(lock, [this] {
                return !_running || !_tasks[0].empty() || !_tasks[1].empty() || !_tasks[2].empty();
            });
            if (!_running && _tasks[0].empty() && _tasks[1].empty() && _tasks[2].empty())
                return;
            int index = 0;
            if (_tasks[0].empty() || (_highBurst >= 8 &&
                (!_tasks[1].empty() || !_tasks[2].empty()))) {
                index = !_tasks[1].empty() &&
                    (_normalBurst < 8 || _tasks[2].empty()) ? 1 : 2;
                _highBurst = 0;
            } else ++_highBurst;
            if (index == 1) ++_normalBurst;
            if (index == 2) _normalBurst = 0;
            task = std::move(_tasks[index].front());
            _tasks[index].pop_front();
            ++_active;
        }
        if (const char* delay = std::getenv("CHAT_PHASE8_TEST_DB_WORKER_DELAY_MS")) {
            try {
                const auto ms = std::stoul(delay);
                if (ms <= 1000) std::this_thread::sleep_for(std::chrono::milliseconds(ms));
            } catch (...) { /* invalid test hook is ignored */ }
        }
        try { task(); }
        catch (const std::exception& error) {
            LOG_ERROR << "DBThreadPool task failed: " << error.what();
        }
        catch (...) { LOG_ERROR << "DBThreadPool task failed: unknown exception"; }
        {
            std::lock_guard<std::mutex> lock(_mutex);
            --_active;
        }
    }
}

void DBThreadPool::shutdown()
{
    {
        std::lock_guard<std::mutex> lg(_mutex);
        _running = false;
    }
    _cv.notify_all();

    for (auto& w : _workers)
    {
        if (w.joinable()) w.join();
    }
    _workers.clear();
    LOG_INFO << "DBThreadPool: shutdown complete";
}
