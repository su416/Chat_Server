cmake_minimum_required(VERSION 3.16)
project(mili VERSION 0.1.0 LANGUAGES CXX
    DESCRIPTION "C++20 low-latency utility library"
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ---- Header-only 主库 ----
add_library(mili INTERFACE)
target_include_directories(mili INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

# ---- Tests（需要 Google Test）----
find_package(GTest REQUIRED)
enable_testing()

macro(add_mili_test name)
    add_executable(${name} tests/${name}.cpp)
    target_link_libraries(${name} PRIVATE mili GTest::gtest GTest::gtest_main)
    add_test(NAME ${name} COMMAND ${name})
endmacro()

add_mili_test(test_arena)
add_mili_test(test_spsc_queue)

# ---- Benchmarks（可选，需要 Google Benchmark）----
find_package(benchmark QUIET)
if(benchmark_FOUND)
    macro(add_mili_bench name)
        add_executable(${name} benchmarks/${name}.cpp)
        target_link_libraries(${name} PRIVATE mili benchmark::benchmark pthread)
    endmacro()
    add_mili_bench(bench_arena)
    add_mili_bench(bench_spsc)
endif()
