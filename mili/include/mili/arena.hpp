#pragma once
// mili/arena — Stack-based bump allocator
//
// 使用场景：请求级临时数据（JSON 序列化、字符串拼接）。
// login() 响应拼装时 23 次 malloc → 1 次。
//
// 和 std::pmr::monotonic_buffer_resource 的区别：
//   - header-only，零虚函数，不需要 RTTI
//   - alignas 标准兼容 std::allocator 接口
//   - 编译期检查 alignment，未对齐的 alloc 不会静默产生错误偏移

#include <cstddef>
#include <cstdint>
#include <new>
#include <type_traits>

namespace mili {

class Arena {         
public:
    Arena() = default;

    // 初始容量建议 = 请求最大临时数据量 × 1.5
    explicit Arena(size_t initialCapacity)
        : _buf(new char[initialCapacity])
        , _capacity(initialCapacity) {}

    ~Arena() { delete[] _buf; }

    Arena(const Arena&) = delete;
    Arena& operator=(const Arena&) = delete;
    Arena(Arena&&) = delete;
    Arena& operator=(Arena&&) = delete;

    // 分配 n 字节，alignment 默认自然对齐
    void* alloc(size_t n, size_t alignment = alignof(std::max_align_t)) noexcept
    {
        uintptr_t addr = reinterpret_cast<uintptr_t>(_buf + _offset);
        size_t padding = (alignment - (addr % alignment)) % alignment;

        if (_offset + padding + n > _capacity) return nullptr;  // 耗尽
        _offset += padding;
        void* p = _buf + _offset;
        _offset += n;
        return p;
    }

    // 重置：内存不释放，下次请求从 0 开始
    void reset() noexcept { _offset = 0; }
    size_t used() const noexcept { return _offset; }
    size_t capacity() const noexcept { return _capacity; }

    // ---- std::allocator 适配 ----
    template <typename T>
    struct Allocator {
        using value_type = T;
        Arena* arena;

        Allocator(Arena& a) noexcept : arena(&a) {}

        template <typename U>
        Allocator(const Allocator<U>& other) noexcept : arena(other.arena) {}

        [[nodiscard]] T* allocate(size_t n) {
            void* p = arena->alloc(n * sizeof(T), alignof(T));
            if (!p) throw std::bad_alloc();
            return static_cast<T*>(p);
        }

        void deallocate(T*, size_t) noexcept {}  // Arena 不单独释放

        template <typename U>
        bool operator==(const Allocator<U>& other) const noexcept {
            return arena == other.arena;
        }
    };

private:
    char* _buf = nullptr;
    size_t _offset = 0;
    size_t _capacity = 0;
};

} // namespace mili
