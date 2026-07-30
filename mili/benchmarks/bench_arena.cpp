// arena vs malloc vs std::pmr — 字符串拼接场景
// 模拟 QuickChat login() 响应拼装时的 23 次临时字符串分配
//
// Build: cmake .. -DCMAKE_BUILD_TYPE=Release && make bench_arena
// Run:   ./bench_arena --benchmark_min_time=0.5

#include <benchmark/benchmark.h>
#include <string>
#include <vector>
#include <memory_resource>
#include "mili/arena.hpp"

using mili::Arena;

// 模拟 login() 中响应拼装的字符串操作
static std::string buildResponse_json_default(int n) {
    std::string result = "{\"msgid\":2,\"errno\":0,\"id\":";
    result += std::to_string(n);
    result += ",\"name\":\"";
    result += "user_" + std::to_string(n);
    result += "\",\"friends\":[";
    for (int i = 0; i < 5; ++i) {
        if (i > 0) result += ",";
        result += "{\"id\":" + std::to_string(i) + ",\"name\":\"f" + std::to_string(i) + "\"}";
    }
    result += "],\"groups\":[]}";
    return result;
}

// mili::Arena 版本
static std::string buildResponse_arena(const Arena& a, int n) {
    using Alloc = Arena::Allocator<char>;
    Alloc alloc(const_cast<Arena&>(a));
    using AS = std::basic_string<char, std::char_traits<char>, Alloc>;

    AS result(alloc);
    result = "{\"msgid\":2,\"errno\":0,\"id\":";
    result += AS(std::to_string(n), alloc);
    result += ",\"name\":\"";
    result += AS("user_" + std::to_string(n), alloc);
    result += "\",\"friends\":[";
    for (int i = 0; i < 5; ++i) {
        if (i > 0) result += ",";
        result += AS("{\"id\":" + std::to_string(i) + ",\"name\":\"f" + std::to_string(i) + "\"}", alloc);
    }
    result += "],\"groups\":[]}";
    return std::string(result);  // 拷贝到标准 string 做公平比较
}

static void BM_JsonDefault(benchmark::State& state) {
    for (auto _ : state) {
        auto s = buildResponse_json_default(42);
        benchmark::DoNotOptimize(s);
    }
}
BENCHMARK(BM_JsonDefault);

static void BM_JsonArena(benchmark::State& state) {
    Arena a(4096);
    for (auto _ : state) {
        a.reset();
        auto s = buildResponse_arena(a, 42);
        benchmark::DoNotOptimize(s);
    }
}
BENCHMARK(BM_JsonArena);

BENCHMARK_MAIN();
