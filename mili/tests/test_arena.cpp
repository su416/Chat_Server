#include <gtest/gtest.h>
#include <string>
#include <vector>
#include "mili/arena.hpp"

using mili::Arena;

TEST(ArenaTest, BasicAlloc)
{
    Arena a(1024);
    void* p = a.alloc(64);
    EXPECT_NE(p, nullptr);
    EXPECT_EQ(a.used(), 64u);
}

TEST(ArenaTest, ResetReuse)
{
    Arena a(1024);
    void* p1 = a.alloc(100);
    a.reset();
    void* p2 = a.alloc(100);
    EXPECT_EQ(p1, p2);  // 同一地址，证明重用
    EXPECT_EQ(a.used(), 100u);
}

TEST(ArenaTest, ExhaustReturnsNull)
{
    Arena a(64);
    void* p = a.alloc(65);
    EXPECT_EQ(p, nullptr);
}

TEST(ArenaTest, AlignmentPadding)
{
    Arena a(256);
    a.alloc(1);                     // offset = 1
    void* p = a.alloc(8, alignof(uint64_t));  // 需要对齐到 8
    EXPECT_EQ(reinterpret_cast<uintptr_t>(p) % 8, 0u);
}

TEST(ArenaTest, StdAllocatorAdapter)
{
    Arena a(4096);
    auto alloc = Arena::Allocator<char>(a);

    // 用 arena 分配 std::string
    using ArenaString = std::basic_string<char, std::char_traits<char>,
                                          Arena::Allocator<char>>;
    ArenaString s(alloc);
    s = "hello arena";
    EXPECT_EQ(s, "hello arena");
    // 注意：s 的生命周期必须 <= Arena a
}

TEST(ArenaTest, MultipleStringsZeroMalloc)
{
    // 这个测试不会直接验证 malloc 次数——用 benchmark 做。
    // 这里只验证功能正确性。
    Arena a(4096);
    auto alloc = Arena::Allocator<char>(a);
    using ArenaString = std::basic_string<char, std::char_traits<char>,
                                          Arena::Allocator<char>>;

    ArenaString s1("hello", alloc);
    ArenaString s2("world", alloc);
    ArenaString s3 = s1 + " " + s2 + "!";

    EXPECT_EQ(s3, "hello world!");
}
