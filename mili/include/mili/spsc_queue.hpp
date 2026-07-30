#pragma once
// mili/spsc_queue — Lock-free Single-Producer Single-Consumer ring buffer
//
// 使用场景：DBThreadPool 任务投递。Muduo 线程生产，DB 线程消费。
// 不需要 mutex + condition_variable 的阻塞语义——有任务就跑，没任务就跳过。
//
// 和 std::queue + std::mutex 的区别：
//   - 零锁竞争：只用 atomic + memory order，无系统调用
//   - Cache-line padding：防止 false sharing
//   - 固定容量：生产者满时返回 false（不阻塞）

#include <atomic>
#include <cstddef>
#include <type_traits>
#include <utility>
#include <new>

namespace mili {

template <typename T, size_t Capacity = 256>
class spsc_queue {
    static_assert((Capacity & (Capacity - 1)) == 0,
                  "Capacity must be power of 2");
public:
    spsc_queue() = default;

    // 生产者：放一个元素。满时返回 false
    bool try_push(T&& val) noexcept
    {
        size_t head = _head.load(std::memory_order_relaxed);
        size_t next = (head + 1) & _mask;
        if (next == _tail_cache) {
            _tail_cache = _tail.load(std::memory_order_acquire);
            if (next == _tail_cache) return false;  // full
        }
        new (&_slots[head]) T(std::move(val));
        _head.store(next, std::memory_order_release);
        return true;
    }

    // 消费者：取一个元素。空时返回 false
    bool try_pop(T& out) noexcept
    {
        size_t tail = _tail.load(std::memory_order_relaxed);
        if (tail == _head_cache) {
            _head_cache = _head.load(std::memory_order_acquire);
            if (tail == _head_cache) return false;  // empty
        }
        out = std::move(_slots[tail]);
        _slots[tail].~T();
        _tail.store((tail + 1) & _mask, std::memory_order_release);
        return true;
    }

    size_t capacity() const noexcept { return Capacity; }

private:
    static constexpr size_t _mask = Capacity - 1;

    // 防止 false sharing：head/tail 各占独立 cache line
    alignas(64) std::atomic<size_t> _head{0};
    size_t _tail_cache{0};  // producer-local copy of tail

    alignas(64) std::atomic<size_t> _tail{0};
    size_t _head_cache{0};  // consumer-local copy of head

    // 存储区，和计数器分离避免同一 cache line 被争抢
    union Slot { T value; Slot() {} ~Slot() {} } _slots[Capacity];
};

} // namespace mili
