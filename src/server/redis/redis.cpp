#include "redis.hpp"
#include "config.hpp"
#include <muduo/base/Logging.h>
#include <iostream>
using namespace std;

Redis::Redis()
    : _publish_context(nullptr), _subcribe_context(nullptr)
{
}

Redis::~Redis()
{
    if (_publish_context != nullptr)
    {
        redisFree(_publish_context);
    }

    if (_subcribe_context != nullptr)
    {
        redisFree(_subcribe_context);
    }
}



bool Redis::connect()
{
    string host = Config::instance().getRedisHost();
    uint16_t port = Config::instance().getRedisPort();

    // 负责 publish 发布消息的上下文连接
    _publish_context = redisConnect(host.c_str(), port);
    if (nullptr == _publish_context)
    {
        LOG_ERROR << "connect redis failed! host=" << host << " port=" << port;
        return false;
    }

    // 负责 subscribe 订阅消息的上下文连接
    _subcribe_context = redisConnect(host.c_str(), port);
    if (nullptr == _subcribe_context)
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
    return _publish_context != nullptr && _publish_context->err == REDIS_OK;
}

// 向redis指定的通道channel发布消息
bool Redis::publish(int channel, string message)
{
    if (!isConnected())
    {
        LOG_ERROR << "redis publish skipped: not connected";
        return false;
    }

    redisReply *reply = (redisReply *)redisCommand(_publish_context, "PUBLISH %d %s", channel, message.c_str());
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
    if (_subcribe_context == nullptr)
    {
        LOG_ERROR << "redis subscribe skipped: not connected";
        return false;
    }

    if (REDIS_ERR == redisAppendCommand(this->_subcribe_context, "SUBSCRIBE %d", channel))
    {
        LOG_ERROR << "subscribe command failed!";
        return false;
    }
    int done = 0;
    while (!done)
    {
        if (REDIS_ERR == redisBufferWrite(this->_subcribe_context, &done))
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
    if (_subcribe_context == nullptr)
    {
        return false;
    }

    if (REDIS_ERR == redisAppendCommand(this->_subcribe_context, "UNSUBSCRIBE %d", channel))
    {
        LOG_ERROR << "unsubscribe command failed!";
        return false;
    }
    int done = 0;
    while (!done)
    {
        if (REDIS_ERR == redisBufferWrite(this->_subcribe_context, &done))
        {
            LOG_ERROR << "unsubscribe command failed!";
            return false;
        }
    }
    return true;
}

// 在独立线程中接收订阅通道中的消息
void Redis::observer_channel_message()
{
    redisReply *reply = nullptr;
    while (REDIS_OK == redisGetReply(this->_subcribe_context, (void **)&reply))
    {
        // 订阅收到的消息是一个带三元素的数组：
        // element[0]="message", element[1]=通道号, element[2]=消息体
        if (reply != nullptr
            && reply->type == REDIS_REPLY_ARRAY
            && reply->elements >= 3
            && reply->element[1] != nullptr && reply->element[1]->str != nullptr
            && reply->element[2] != nullptr && reply->element[2]->str != nullptr)
        {
            // 给业务层上报通道上发生的消息
            _notify_message_handler(atoi(reply->element[1]->str) , reply->element[2]->str);
        }

        freeReplyObject(reply);
    }

    LOG_ERROR << ">>>>>>>>>>>>> observer_channel_message quit <<<<<<<<<<<<<";
}

void Redis::init_notify_handler(function<void(int,string)> fn)
{
    this->_notify_message_handler = fn;
}