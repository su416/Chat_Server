#include "db_thread_pool.hpp"
#include <gtest/gtest.h>

#include <condition_variable>
#include <future>
#include <mutex>
#include <vector>

TEST(DBThreadPool, BoundedQueueAndPriorityReserve) {
    auto* pool = DBThreadPool::instance();
    pool->init(1, 8, 2);
    std::mutex gateMutex;
    std::condition_variable gateCv;
    bool release = false;
    std::promise<void> started;
    ASSERT_EQ(pool->trySubmit([&] {
        started.set_value();
        std::unique_lock<std::mutex> lock(gateMutex);
        gateCv.wait(lock, [&] { return release; });
    }, DBThreadPool::Priority::High), DBThreadPool::SubmitResult::Accepted);
    started.get_future().wait();

    std::vector<int> order;
    for (int i = 0; i < 4; ++i)
        EXPECT_EQ(pool->trySubmit([&order] { order.push_back(3); },
            DBThreadPool::Priority::Low), DBThreadPool::SubmitResult::Accepted);
    EXPECT_EQ(pool->trySubmit([] {}, DBThreadPool::Priority::Low),
              DBThreadPool::SubmitResult::QueueFull);
    for (int i = 0; i < 2; ++i)
        EXPECT_EQ(pool->trySubmit([&order] { order.push_back(2); },
            DBThreadPool::Priority::Normal), DBThreadPool::SubmitResult::Accepted);
    EXPECT_EQ(pool->trySubmit([] {}, DBThreadPool::Priority::Normal),
              DBThreadPool::SubmitResult::QueueFull);
    for (int i = 0; i < 2; ++i)
        EXPECT_EQ(pool->trySubmit([&order] { order.push_back(1); },
            DBThreadPool::Priority::High), DBThreadPool::SubmitResult::Accepted);
    EXPECT_EQ(pool->trySubmit([] {}, DBThreadPool::Priority::High),
              DBThreadPool::SubmitResult::QueueFull);
    auto metrics = pool->metrics();
    EXPECT_EQ(metrics.depth, 8u);
    EXPECT_EQ(metrics.capacity, 8u);
    EXPECT_EQ(metrics.activeWorkers, 1u);
    EXPECT_GE(metrics.rejected, 3u);
    {
        std::lock_guard<std::mutex> lock(gateMutex);
        release = true;
    }
    gateCv.notify_one();
    pool->shutdown();
    ASSERT_EQ(order.size(), 8u);
    EXPECT_EQ(order.front(), 1); // ACK-class work gets first slot after blocker.
    EXPECT_EQ(pool->metrics().depth, 0u);
    EXPECT_EQ(pool->trySubmit([] {}), DBThreadPool::SubmitResult::Stopped);
}
