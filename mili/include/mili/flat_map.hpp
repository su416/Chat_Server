#pragma once
// mili/flat_map — Open-addressing hash map for small key-value tables
//
// 使用场景：_userConnMap（在线用户表，通常 < 1000 人）。
// std::unordered_map 使用拉链法（指针跳转），cache miss 率高。
// flat_map 用开放寻址，所有数据在连续内存中，L3 cache 友好。
//
// 和 std::unordered_map 的区别：
//   - 开放寻址（linear probing），无指针跳转
//   - tombstone 标记删除，不立即缩容
//   - 迭代顺序不保证
//   - 不支持 const Key 的 mutable value

#include <cstddef>
#include <cstdint>
#include <vector>
#include <functional>
#include <utility>

namespace mili {

template <typename Key, typename Value, typename Hash = std::hash<Key>>
class flat_map {
    enum State : uint8_t { EMPTY = 0, OCCUPIED = 1, TOMBSTONE = 2 };

    struct Slot {
        State state = EMPTY;
        Key key{};
        Value value{};
    };

public:
    using iterator = Slot*;

    flat_map() { _slots.resize(16); }

    // ---- 查找 ----
    Value* find(const Key& key) {
        size_t idx = _hash(key) & _mask();
        for (size_t i = 0; i < _slots.size(); ++i) {
            auto& s = _slots[idx];
            if (s.state == EMPTY) return nullptr;
            if (s.state == OCCUPIED && s.key == key) return &s.value;
            idx = (idx + 1) & _mask();
        }
        return nullptr;
    }

    // ---- 插入 ----
    void insert(const Key& key, const Value& value) {
        if (_size * 2 >= _slots.size()) _rehash(_slots.size() * 2);
        _insert_impl(key, value);
    }

    void insert(Key&& key, Value&& value) {
        if (_size * 2 >= _slots.size()) _rehash(_slots.size() * 2);
        _insert_impl(std::move(key), std::move(value));
    }

    // ---- 删除 ----
    void erase(const Key& key) {
        size_t idx = _hash(key) & _mask();
        for (size_t i = 0; i < _slots.size(); ++i) {
            auto& s = _slots[idx];
            if (s.state == EMPTY) return;
            if (s.state == OCCUPIED && s.key == key) {
                s.state = TOMBSTONE;
                --_size;
                return;
            }
            idx = (idx + 1) & _mask();
        }
    }

    // ---- 容量 ----
    size_t size() const noexcept { return _size; }
    bool empty() const noexcept { return _size == 0; }

private:
    size_t _mask() const noexcept { return _slots.size() - 1; }

    template <typename K, typename V>
    void _insert_impl(K&& key, V&& value) {
        size_t idx = _hash(key) & _mask();
        for (size_t i = 0; i < _slots.size(); ++i) {
            auto& s = _slots[idx];
            if (s.state != OCCUPIED) {
                s.state = OCCUPIED;
                s.key = std::forward<K>(key);
                s.value = std::forward<V>(value);
                ++_size;
                return;
            }
            if (s.key == key) {
                s.value = std::forward<V>(value);  // 覆盖
                return;
            }
            idx = (idx + 1) & _mask();
        }
    }

    void _rehash(size_t newCap) {
        std::vector<Slot> old = std::move(_slots);
        _slots.resize(newCap);
        _size = 0;
        for (auto& s : old) {
            if (s.state == OCCUPIED)
                _insert_impl(std::move(s.key), std::move(s.value));
        }
    }

    std::vector<Slot> _slots;
    size_t _size = 0;
    Hash _hash;
};

} // namespace mili
