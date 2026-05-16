#include "chatserver.hpp"
#include "chatservice.hpp"
#include "config.hpp"
#include "connectionpool.h"
#include "rate_limiter.hpp"
#include "db_thread_pool.hpp"
#include <iostream>
#include <signal.h>
#include "chat.pb.h"

using namespace std;

// 处理服务器 ctrl+c 结束后，重置 user 的状态信息
void resetHandler(int)
{
    ChatService::instance()->reset();
    exit(0);
}

int main(int argc, char **argv)
{
    // 加载配置文件
    if (!Config::instance().load("conf/chatserver.json"))
    {
        cerr << "load config file failed!" << endl;
        return -1;
    }

    // 初始化数据库连接池
    {
        auto& cfg = Config::instance();
        int poolSize = cfg.getThreadNum();  // 一线程一连接
        ConnectionPool::instance()->init(
            cfg.getMySQLHost(), cfg.getMySQLPort(),
            cfg.getMySQLUser(), cfg.getMySQLPassword(),
            cfg.getMySQLDBName(), poolSize
        );
    }

    // 初始化令牌桶限流器（参数由配置文件控制）
    {
        auto& cfg = Config::instance();
        if (cfg.isRateLimitEnabled())
        {
            RateLimiter::instance()->init(
                cfg.getRateLimitCapacity(),
                cfg.getRateLimitPerSec()
            );
        }
    }

    // 初始化 DB 线程池（用于异步 DB 写入，线程数 = Muduo 线程数）
    DBThreadPool::instance()->init(Config::instance().getThreadNum());

    signal(SIGINT, resetHandler);

    EventLoop loop;
    InetAddress addr(
        Config::instance().getServerIp(),
        Config::instance().getServerPort()
    );

    ChatServer server(
        &loop,
        addr,
        Config::instance().getServerName(),
        Config::instance().getThreadNum()
    );

    server.start();
    loop.loop();

    return 0;
}
