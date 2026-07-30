#include <gtest/gtest.h>
#include <thread>
#include <vector>
#include "mili/spsc_queue.hpp"

using mili::spsc_queue;

TEST(SpscQueueTest, PushPopBasic)
{
    spsc_queue<int, 4> q;
    EXPECT_TRUE(q.try_push(42));
    int out = 0;
    EXPECT_TRUE(q.try_pop(out));
    EXPECT_EQ(out, 42);
}

TEST(SpscQueueTest, EmptyPopReturnsFalse)
{
    spsc_queue<int, 4> q;
    int out;
    EXPECT_FALSE(q.try_pop(out));
}

TEST(SpscQueueTest, FullPushReturnsFalse)
{
    spsc_queue<int, 4> q;  // Capacity=4, usable=3 (one slot reserved for full/empty distinction)
    EXPECT_TRUE(q.try_push(1));
    EXPECT_TRUE(q.try_push(2));
    EXPECT_TRUE(q.try_push(3));
    EXPECT_FALSE(q.try_push(4));
}

TEST(SpscQueueTest, SingleThreadRoundtrip)
{
    spsc_queue<int, 8> q;
    for (int i = 0; i < 7; ++i)  // usable = capacity - 1
        EXPECT_TRUE(q.try_push(i * 10));
    for (int i = 0; i < 7; ++i) {
        int out;
        EXPECT_TRUE(q.try_pop(out));
        EXPECT_EQ(out, i * 10);
    }
}

TEST(SpscQueueTest, Threaded)
{
    spsc_queue<int, 64> q;
    std::atomic<bool> done{false};
    std::vector<int> received;

    std::thread consumer([&] {
        while (!done.load(std::memory_order_relaxed)) {
            int val;
            if (q.try_pop(val))
                received.push_back(val);
        }
        // drain remaining
        int val;
        while (q.try_pop(val))
            received.push_back(val);
    });

    for (int i = 0; i < 100; ++i) {
        while (!q.try_push(i))
            ;  // spin until space available
    }
    done.store(true, std::memory_order_relaxed);
    consumer.join();

    EXPECT_EQ(received.size(), 100u);
}
