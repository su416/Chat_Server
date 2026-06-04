#ifndef REDIS_H
#define REDIS_H

#include <hiredis/hiredis.h>
#include <thread>
#include <functional>
#include <memory>
#include <mutex>
using namespace std;

// hiredis context 自动释放器
struct RedisContextDeleter {
    void operator()(redisContext* ctx) const {
        if (ctx) redisFree(ctx);
    }
};
using RedisContextPtr = unique_ptr<redisContext, RedisContextDeleter>;

class Redis
{
public:
    Redis();
    ~Redis() = default;  // unique_ptr 自动释放

    // 连接redis服务器
    bool connect();

    // 检查连接状态
    bool isConnected() const;

    // 向redis指定的通道channel发布消息
    bool publish(int channel, string message);

    // 向redis指定的通道subscribe订阅消息
    bool subscribe(int channel);

    // 向redis指定的通道unsubscribe取消订阅消息
    bool unsubscribe(int channel);

    // 在独立线程中接收订阅通道中的消息
    void observer_channel_message();

    // 初始化向业务层上报通道消息的回调对象
    void init_notify_handler(function<void(int, string)> fn);

    // 设置重连后回调（observer 重连成功后触发，用于批量恢复订阅）
    void setReconnectHandler(function<void()> fn);

    // 服务发现：注册 ChatServer 实例到 Redis
    bool registerServer(const string& name, const string& host, int port);
    // 更新当前连接数
    bool updateConnections(const string& name, int count);

private:
    // 重新连接两个上下文
    bool reconnect();

    // hiredis同步上下文对象，负责publish消息（unique_ptr 自动释放）
    RedisContextPtr _publish_context;

    // hiredis同步上下文对象，负责subscribe消息（unique_ptr 自动释放）
    RedisContextPtr _subcribe_context;

    // 保护 _subcribe_context 的并发访问（observer 线程 + Muduo 线程）
    mutex _subscribeMutex;

    // 回调操作，收到订阅的消息，给service层上报
    function<void(int, string)> _notify_message_handler;

    // 重连后回调，用于批量恢复订阅
    function<void()> _onReconnect;
};

#endif
