#include "redis.hpp"
#include "config.hpp"

#include <muduo/base/Logging.h>
#include <chrono>
#include <ctime>
#include <sys/socket.h>

namespace {
RedisContextPtr makeContext()
{
    const auto& config = Config::instance();
    timeval timeout{1, 0};
    return RedisContextPtr(redisConnectWithTimeout(
        config.getRedisHost().c_str(), config.getRedisPort(), timeout));
}
}

Redis::~Redis()
{
    stop();
}

void Redis::stop()
{
    {
        std::lock_guard<std::mutex> lock(_observerMutex);
        _stopping = true;
        if (_subscriberFd >= 0) ::shutdown(_subscriberFd, SHUT_RDWR);
    }
    if (_observer.joinable()) _observer.join();
}

bool Redis::connectCommandLocked()
{
    _commandContext = makeContext();
    if (!_commandContext || _commandContext->err)
    {
        LOG_ERROR << "Redis command connection unavailable";
        return false;
    }
    redisSetTimeout(_commandContext.get(), timeval{1, 0});
    return true;
}

bool Redis::connect()
{
    std::lock_guard<std::mutex> lock(_commandMutex);
    return connectCommandLocked();
}

bool Redis::isConnected() const
{
    std::lock_guard<std::mutex> lock(_commandMutex);
    return _commandContext && !_commandContext->err;
}

RedisReplyPtr Redis::command(const std::vector<std::string>& args)
{
    if (args.empty()) return {};
    std::lock_guard<std::mutex> lock(_commandMutex);
    if ((!_commandContext || _commandContext->err) && !connectCommandLocked()) return {};
    std::vector<const char*> argv;
    std::vector<size_t> lengths;
    argv.reserve(args.size());
    lengths.reserve(args.size());
    for (const auto& arg : args)
    {
        argv.push_back(arg.data());
        lengths.push_back(arg.size());
    }
    RedisReplyPtr reply(static_cast<redisReply*>(redisCommandArgv(
        _commandContext.get(), static_cast<int>(argv.size()), argv.data(), lengths.data())));
    if (!reply) LOG_ERROR << "Redis command failed: " << args.front();
    else if (reply->type == REDIS_REPLY_ERROR) LOG_ERROR << "Redis error: " << reply->str;
    return reply;
}

std::vector<RedisReplyPtr> Redis::batch(
    const std::vector<std::vector<std::string>>& commands)
{
    std::vector<RedisReplyPtr> replies;
    if (commands.empty()) return replies;
    std::lock_guard<std::mutex> lock(_commandMutex);
    if ((!_commandContext || _commandContext->err) && !connectCommandLocked()) return {};
    for (const auto& args : commands)
    {
        std::vector<const char*> argv;
        std::vector<size_t> lengths;
        argv.reserve(args.size());
        lengths.reserve(args.size());
        for (const auto& arg : args)
        {
            argv.push_back(arg.data());
            lengths.push_back(arg.size());
        }
        if (argv.empty() || redisAppendCommandArgv(_commandContext.get(),
                static_cast<int>(argv.size()), argv.data(), lengths.data()) != REDIS_OK)
            return {};
    }
    replies.reserve(commands.size());
    for (std::size_t index = 0; index < commands.size(); ++index)
    {
        redisReply* raw = nullptr;
        if (redisGetReply(_commandContext.get(), reinterpret_cast<void**>(&raw)) != REDIS_OK)
            return {};
        replies.emplace_back(raw);
    }
    return replies;
}

bool Redis::publish(const std::string& channel, const std::string& message)
{
    auto reply = command({"PUBLISH", channel, message});
    // 0 listeners means no node accepted this ephemeral route.
    return reply && reply->type == REDIS_REPLY_INTEGER && reply->integer > 0;
}

bool Redis::subscribeNode(const std::string& channel,
                          std::function<void(const std::string&)> handler)
{
    {
        std::lock_guard<std::mutex> lock(_observerMutex);
        if (_observer.joinable() || channel.empty() || !handler) return false;
        _channel = channel;
        _handler = std::move(handler);
    }
    _observer = std::thread(&Redis::observe, this);
    std::unique_lock<std::mutex> lock(_observerMutex);
    return _observerReady.wait_for(lock, std::chrono::seconds(3),
                                   [this] { return _subscribed; });
}

void Redis::observe()
{
    while (true)
    {
        {
            std::lock_guard<std::mutex> lock(_observerMutex);
            if (_stopping) return;
            _subscribed = false;
        }
        auto context = makeContext();
        if (context && !context->err)
        {
            {
                std::lock_guard<std::mutex> lock(_observerMutex);
                _subscriberFd = context->fd;
            }
            RedisReplyPtr subscribed(static_cast<redisReply*>(
                redisCommand(context.get(), "SUBSCRIBE %s", _channel.c_str())));
            if (subscribed && subscribed->type == REDIS_REPLY_ARRAY)
            {
                {
                    std::lock_guard<std::mutex> lock(_observerMutex);
                    _subscribed = true;
                }
                _observerReady.notify_all();
                while (true)
                {
                    {
                        std::lock_guard<std::mutex> lock(_observerMutex);
                        if (_stopping) return;
                    }
                    redisReply* raw = nullptr;
                    if (redisGetReply(context.get(), reinterpret_cast<void**>(&raw)) != REDIS_OK)
                        break;
                    RedisReplyPtr reply(raw);
                    if (reply && reply->type == REDIS_REPLY_ARRAY && reply->elements == 3 &&
                        reply->element[0] && reply->element[0]->str &&
                        std::string(reply->element[0]->str) == "message" &&
                        reply->element[2] && reply->element[2]->str)
                    {
                        try {
                            _handler(std::string(reply->element[2]->str, reply->element[2]->len));
                        } catch (const std::exception& error) {
                            LOG_ERROR << "Redis routed callback rejected: " << error.what();
                        }
                    }
                }
            }
        }
        {
            std::lock_guard<std::mutex> lock(_observerMutex);
            _subscriberFd = -1;
        }
        LOG_WARN << "Redis node subscriber disconnected; retrying";
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

bool Redis::registerServer(const std::string& name, const std::string& host, int port)
{
    const std::string value = "{\"host\":\"" + host + "\",\"port\":" +
        std::to_string(port) + ",\"connections\":0,\"updated_at\":" +
        std::to_string(std::time(nullptr)) + "}";
    auto reply = command({"HSET", "chatservers", name, value});
    return reply && reply->type == REDIS_REPLY_INTEGER;
}

bool Redis::updateConnections(const std::string& name, int count)
{
    auto reply = command({"HGET", "chatservers", name});
    if (!reply || reply->type != REDIS_REPLY_STRING) return false;
    std::string value(reply->str, reply->len);
    auto replaceNumber = [&](const std::string& key, const std::string& number) {
        const auto pos = value.find("\"" + key + "\":");
        if (pos == std::string::npos) return;
        const auto begin = value.find_first_of("0123456789", pos + key.size() + 3);
        if (begin == std::string::npos) return;
        const auto end = value.find_first_not_of("0123456789", begin);
        value.replace(begin, end == std::string::npos ? std::string::npos : end - begin, number);
    };
    replaceNumber("connections", std::to_string(count));
    replaceNumber("updated_at", std::to_string(std::time(nullptr)));
    reply = command({"HSET", "chatservers", name, value});
    return reply && reply->type == REDIS_REPLY_INTEGER;
}
