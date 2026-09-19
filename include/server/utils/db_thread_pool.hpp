#ifndef DB_THREAD_POOL_HPP
#define DB_THREAD_POOL_HPP

#include <deque>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <thread>
#include <vector>
#include <array>
#include <atomic>
#include <cstdint>

// 数据库专用线程池
// Muduo I/O 线程只做非阻塞提交；队列按优先级和总量硬限制。
class DBThreadPool
{
public:
    enum class Priority { High = 0, Normal = 1, Low = 2 };
    enum class SubmitResult { Accepted, QueueFull, Stopped };
    struct Metrics {
        std::size_t depth = 0, capacity = 0, peak = 0;
        std::size_t activeWorkers = 0;
        std::uint64_t rejected = 0;
    };
    static DBThreadPool* instance();

    // 初始化：启动 threadCount 个后台线程
    void init(int threadCount, std::size_t maxQueued = 1024,
              std::size_t reservedHigh = 256);

    // I/O loop 只能走非阻塞提交；高优先级保留容量供 ACK/新投递。
    SubmitResult trySubmit(std::function<void()> task,
                           Priority priority = Priority::Normal);
    SubmitResult submitVoid(std::function<void()> task,
                            Priority priority = Priority::Low)
    { return trySubmit(std::move(task), priority); }
    bool trySubmitDurable(std::function<void()> task,
                          Priority priority = Priority::Normal)
    { return trySubmit(std::move(task), priority) == SubmitResult::Accepted; }
    Metrics metrics() const;

    void shutdown();

private:
    DBThreadPool() = default;

    void workerLoop();

    std::vector<std::thread> _workers;
    std::array<std::deque<std::function<void()>>, 3> _tasks;
    mutable std::mutex _mutex;
    std::condition_variable _cv;
    std::size_t _capacity = 1024, _reservedHigh = 256, _peak = 0;
    std::size_t _active = 0;
    std::uint64_t _rejected = 0;
    unsigned _highBurst = 0, _normalBurst = 0;
    bool _running = false;
};

#endif
