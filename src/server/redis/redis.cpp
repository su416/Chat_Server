#include "redis.hpp"
#include "config.hpp"
#include <muduo/base/Logging.h>
#include <chrono>
#include <thread>
#include <iostream>
using namespace std;

Redis::Redis()
{
}



bool Redis::connect()
{
    string host = Config::instance().getRedisHost();
    uint16_t port = Config::instance().getRedisPort();

    // 负责 publish 发布消息的上下文连接
    _publish_context.reset(redisConnect(host.c_str(), port));
    if (!_publish_context)
    {
        LOG_ERROR << "connect redis failed! host=" << host << " port=" << port;
        return false;
    }

    // 负责 subscribe 订阅消息的上下文连接
    _subcribe_context.reset(redisConnect(host.c_str(), port));
    if (!_subcribe_context)
    {
        LOG_ERROR << "connect redis failed! host=" << host << " port=" << port;
        return false;
    }

    // 在单独的线程中，监听通道上的事件，有消息给业务层进行上报
    thread t([&]() {
        observer_channel_message();
    });
    t.detach();

    LOG_INFO << "connect redis-server success! host=" << host << " port=" << port;

    return true;
}

// 检查 Redis 连接状态
bool Redis::isConnected() const
{
    return _publish_context && _publish_context->err == REDIS_OK;
}

// 向redis指定的通道channel发布消息
bool Redis::publish(int channel, string message)
{
    if (!isConnected())
    {
        LOG_ERROR << "redis publish skipped: not connected";
        return false;
    }

    redisReply *reply = (redisReply *)redisCommand(_publish_context.get(), "PUBLISH %d %s", channel, message.c_str());
    if (nullptr == reply)
    {
        LOG_ERROR << "publish command failed!";
        return false;
    }
    freeReplyObject(reply);
    return true;
}

// 向redis指定的通道subscribe订阅消息
bool Redis::subscribe(int channel)
{
    lock_guard<mutex> lock(_subscribeMutex);

    if (!_subcribe_context)
    {
        LOG_ERROR << "redis subscribe skipped: not connected";
        return false;
    }

    if (REDIS_ERR == redisAppendCommand(this->_subcribe_context.get(), "SUBSCRIBE %d", channel))
    {
        LOG_ERROR << "subscribe command failed!";
        return false;
    }
    int done = 0;
    while (!done)
    {
        if (REDIS_ERR == redisBufferWrite(this->_subcribe_context.get(), &done))
        {
            LOG_ERROR << "subscribe command failed!";
            return false;
        }
    }
    return true;
}

// 向redis指定的通道unsubscribe取消订阅消息
bool Redis::unsubscribe(int channel)
{
    lock_guard<mutex> lock(_subscribeMutex);

    if (!_subcribe_context)
    {
        return false;
    }

    if (REDIS_ERR == redisAppendCommand(this->_subcribe_context.get(), "UNSUBSCRIBE %d", channel))
    {
        LOG_ERROR << "unsubscribe command failed!";
        return false;
    }
    int done = 0;
    while (!done)
    {
        if (REDIS_ERR == redisBufferWrite(this->_subcribe_context.get(), &done))
        {
            LOG_ERROR << "unsubscribe command failed!";
            return false;
        }
    }
    return true;
}

// 在独立线程中接收订阅通道中的消息（断线自动重连）
void Redis::observer_channel_message()
{
    while (true)
    {
        redisReply *reply = nullptr;
        while (_subcribe_context &&
               REDIS_OK == redisGetReply(this->_subcribe_context.get(), (void **)&reply))
        {
            if (reply != nullptr
                && reply->type == REDIS_REPLY_ARRAY
                && reply->elements >= 3
                && reply->element[1] != nullptr && reply->element[1]->str != nullptr
                && reply->element[2] != nullptr && reply->element[2]->str != nullptr)
            {
                _notify_message_handler(atoi(reply->element[1]->str),
                                        reply->element[2]->str);
            }
            freeReplyObject(reply);
        }

        LOG_ERROR << "redis observer lost connection, retrying in 3s...";

        // 等待后尝试重连
        this_thread::sleep_for(chrono::seconds(3));
        if (!reconnect())
        {
            LOG_ERROR << "redis reconnect failed, retrying in 3s...";
            continue;
        }
        LOG_INFO << "redis observer reconnected";

        // 重连成功后触发回调，批量恢复订阅（消除消息丢失窗口）
        if (_onReconnect)
        {
            _onReconnect();
        }                
    }
}

bool Redis::reconnect()
{
    string host = Config::instance().getRedisHost();
    uint16_t port = Config::instance().getRedisPort();

    // 释放旧连接（unique_ptr::reset 自动调 RedisContextDeleter）
    _subcribe_context.reset();
    _publish_context.reset();

    // 重建两个上下文
    _publish_context.reset(redisConnect(host.c_str(), port));
    if (!_publish_context || _publish_context->err)
        return false;

    _subcribe_context.reset(redisConnect(host.c_str(), port));
    if (!_subcribe_context || _subcribe_context->err)
        return false;

    return true;
}

void Redis::init_notify_handler(function<void(int,string)> fn)
{
    this->_notify_message_handler = fn;
}

void Redis::setReconnectHandler(function<void()> fn)
{
    _onReconnect = std::move(fn);
}

bool Redis::registerServer(const string& name, const string& host, int port)
{
    if (!isConnected()) return false;

    string json = "{\"host\":\"" + host + "\",\"port\":" + to_string(port)
                + ",\"connections\":0,\"updated_at\":\"" +
                to_string(time(nullptr)) + "\"}";

    auto* reply = (redisReply*)redisCommand(_publish_context.get(),
        "HSET chatservers %s %s", name.c_str(), json.c_str());
    if (!reply) return false;
    freeReplyObject(reply);
    return true;
}

bool Redis::updateConnections(const string& name, int count)
{
    if (!isConnected()) return false;

    // 先读当前 JSON，更新 connections 字段，再写回
    auto* reply = (redisReply*)redisCommand(_publish_context.get(),
        "HGET chatservers %s", name.c_str());
    if (!reply || reply->type != REDIS_REPLY_STRING) {
        if (reply) freeReplyObject(reply);
        return false;
    }

    string json(reply->str);
    freeReplyObject(reply);

    // 简单字符串替换：更新 connections 和 updated_at
    auto updateField = [](string& s, const string& key, const string& val) {
        auto pos = s.find("\"" + key + "\":");
        if (pos == string::npos) return;
        auto start = s.find_first_of("0123456789", pos + key.size() + 3);
        auto end = s.find_first_not_of("0123456789", start);
        if (start != string::npos && end != string::npos)
            s.replace(start, end - start, val);
    };
    updateField(json, "connections", to_string(count));
    updateField(json, "updated_at", to_string(time(nullptr)));

    reply = (redisReply*)redisCommand(_publish_context.get(),
        "HSET chatservers %s %s", name.c_str(), json.c_str());
    if (!reply) return false;
    freeReplyObject(reply);
    return true;
}