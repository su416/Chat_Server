#ifndef CHAT_REDIS_HPP
#define CHAT_REDIS_HPP

#include <hiredis/hiredis.h>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct RedisContextDeleter {
    void operator()(redisContext* context) const { if (context) redisFree(context); }
};
struct RedisReplyDeleter {
    void operator()(redisReply* reply) const { if (reply) freeReplyObject(reply); }
};
using RedisContextPtr = std::unique_ptr<redisContext, RedisContextDeleter>;
using RedisReplyPtr = std::unique_ptr<redisReply, RedisReplyDeleter>;

// 命令连接由 mutex 串行化；订阅连接只由观察线程使用。
class Redis final
{
public:
    Redis() = default;
    ~Redis();
    Redis(const Redis&) = delete;
    Redis& operator=(const Redis&) = delete;

    bool connect();
    void stop();
    bool isConnected() const;
    RedisReplyPtr command(const std::vector<std::string>& args);
    std::vector<RedisReplyPtr> batch(const std::vector<std::vector<std::string>>& commands);
    bool publish(const std::string& channel, const std::string& message);
    bool subscribeNode(const std::string& channel,
                       std::function<void(const std::string&)> handler);

    // 保留已有负载均衡 Hash；在线归属不依赖它。
    bool registerServer(const std::string& name, const std::string& host, int port);
    bool updateConnections(const std::string& name, int count);

private:
    bool connectCommandLocked();
    void observe();

    mutable std::mutex _commandMutex;
    RedisContextPtr _commandContext;
    std::thread _observer;
    std::mutex _observerMutex;
    std::condition_variable _observerReady;
    std::string _channel;
    std::function<void(const std::string&)> _handler;
    bool _subscribed = false;
    bool _stopping = false;
    int _subscriberFd = -1;
};

#endif
