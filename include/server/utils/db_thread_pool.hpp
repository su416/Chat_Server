#ifndef DB_THREAD_POOL_HPP
#define DB_THREAD_POOL_HPP

#include <queue>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <future>
#include <thread>
#include <vector>

using namespace std;

// 数据库专用线程池
// Muduo IO 线程将 DB 写操作提交到池中立即返回，不阻塞事件循环。
// 读操作如需返回值，通过 future.get() 同步等待。
class DBThreadPool
{
public:
    static DBThreadPool* instance();

    // 初始化：启动 threadCount 个后台线程
    void init(int threadCount);

    // 提交一个任务，返回 future 用于获取结果（或 fire-and-forget）
    template<typename F>
    auto submit(F&& f) -> future<decltype(f())>
    {
        using Ret = decltype(f());
        auto task = make_shared<packaged_task<Ret()>>(forward<F>(f));
        future<Ret> fut = task->get_future();

        {
            lock_guard<mutex> lg(_mutex);
            _tasks.emplace([task]() { (*task)(); });
        }
        _cv.notify_one();

        return fut;
    }

    // 提交一个无返回值的任务（fire-and-forget）
    void submitVoid(function<void()> task);
    // Durable delivery work must fail closed rather than grow an unbounded queue.
    bool trySubmitDurable(function<void()> task, size_t maxQueued = 1024);

    void shutdown();

private:
    DBThreadPool() = default;

    void workerLoop();

    vector<thread> _workers;
    queue<function<void()>> _tasks;
    mutex _mutex;
    condition_variable _cv;
    bool _running = false;
};

#endif
