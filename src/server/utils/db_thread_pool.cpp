#include "db_thread_pool.hpp"
#include <muduo/base/Logging.h>

DBThreadPool* DBThreadPool::instance()
{
    static DBThreadPool pool;
    return &pool;
}

void DBThreadPool::init(int threadCount)
{
    if (_running) return;

    _running = true;
    for (int i = 0; i < threadCount; ++i)
    {
        _workers.emplace_back(&DBThreadPool::workerLoop, this);
    }
    LOG_INFO << "DBThreadPool: started with " << threadCount << " threads";
}

void DBThreadPool::submitVoid(function<void()> task)
{
    {
        lock_guard<mutex> lg(_mutex);
        _tasks.emplace(std::move(task));
    }
    _cv.notify_one();
}

bool DBThreadPool::trySubmitDurable(function<void()> task, size_t maxQueued)
{
    if (!task || maxQueued == 0) return false;
    {
        lock_guard<mutex> lg(_mutex);
        if (!_running || _tasks.size() >= maxQueued) return false;
        _tasks.emplace(std::move(task));
    }
    _cv.notify_one();
    return true;
}

void DBThreadPool::workerLoop()
{
    while (true)
    {
        function<void()> task;
        {
            unique_lock<mutex> lock(_mutex);
            _cv.wait(lock, [this] { return !_tasks.empty() || !_running; });

            if (!_running && _tasks.empty())
                return;

            task = std::move(_tasks.front());
            _tasks.pop();
        }
        try { task(); }
        catch (const std::exception& error) {
            LOG_ERROR << "DBThreadPool task failed: " << error.what();
        }
        catch (...) { LOG_ERROR << "DBThreadPool task failed: unknown exception"; }
    }
}

void DBThreadPool::shutdown()
{
    {
        lock_guard<mutex> lg(_mutex);
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
