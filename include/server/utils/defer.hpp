#ifndef DEFER_HPP
#define DEFER_HPP

#include <functional>

// 作用域守卫：离开作用域时自动执行指定操作（类似 Go 的 defer）
// 用法：Defer defer([] { cleanup(); });
class Defer
{
public:
    explicit Defer(std::function<void()> func) : _func(std::move(func)) {}
    ~Defer() { if (_func) _func(); }

    Defer(const Defer&) = delete;
    Defer& operator=(const Defer&) = delete;

private:
    std::function<void()> _func;
};

#endif
