// spsc_queue vs std::queue + std::mutex — 单生产者单消费者场景
// 模拟 DBThreadPool 任务投递：Muduo 线程 → DB 线程
//
// Build: cmake .. -DCMAKE_BUILD_TYPE=Release && make bench_spsc
// Run:   ./bench_spsc

#include <benchmark/benchmark.h>
#include <queue>
#include <mutex>
#include <thread>
#include <atomic>
#include "mili/spsc_queue.hpp"

using mili::spsc_queue;

// ---- std::queue + std::mutex ----
static void BM_MutexQueue(benchmark::State& state) {
    std::queue<int> q;
    std::mutex m;
    std::atomic<bool> done{false};
    int sum = 0;

    std::thread consumer([&] {
        while (!done.load(std::memory_order_relaxed)) {
            std::lock_guard<std::mutex> lk(m);
            if (!q.empty()) {
                sum += q.front();
                q.pop();
            }
        }
        std::lock_guard<std::mutex> lk(m);
        while (!q.empty()) { sum += q.front(); q.pop(); }
    });

    for (auto _ : state) {
        std::lock_guard<std::mutex> lk(m);
        q.push(1);
        q.push(2);
        q.push(3);
    }

    done.store(true, std::memory_order_relaxed);
    consumer.join();
    benchmark::DoNotOptimize(sum);
}
BENCHMARK(BM_MutexQueue)->Threads(1);

// ---- mili::spsc_queue ----
static void BM_SpscQueue(benchmark::State& state) {
    spsc_queue<int, 64> q;
    std::atomic<bool> done{false};
    int sum = 0;

    std::thread consumer([&] {
        while (!done.load(std::memory_order_relaxed)) {
            int v;
            if (q.try_pop(v)) sum += v;
        }
        int v;
        while (q.try_pop(v)) sum += v;
    });

    for (auto _ : state) {
        while (!q.try_push(1)) ;
        while (!q.try_push(2)) ;
        while (!q.try_push(3)) ;
    }

    done.store(true, std::memory_order_relaxed);
    consumer.join();
    benchmark::DoNotOptimize(sum);
}
BENCHMARK(BM_SpscQueue)->Threads(1);

BENCHMARK_MAIN();
