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

// 全局指针：供信号处理器通知主循环退出
static EventLoop* g_loop = nullptr;

// 处理服务器 ctrl+c —— 优雅关闭
void resetHandler(int)
{
    // 1. 将所有在线用户标记为 offline
    ChatService::instance()->reset();

    // 2. 通知主循环退出（而非 exit(0) 硬杀进程）
    if (g_loop)
    {
        g_loop->quit();
    }
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

    // 触发 ChatService 单例初始化（连接 Redis、注册消息处理器）
    ChatService::instance();

    signal(SIGINT, resetHandler);

    EventLoop loop;
    g_loop = &loop;

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
    loop.loop();  // 阻塞直到 quit() 被调用

    // 优雅关闭：释放所有资源
    DBThreadPool::instance()->shutdown();     // 等待 DB 线程结束
    ConnectionPool::instance()->shutdown();   // 释放 MySQL 连接

    return 0;  // 正常退出，所有析构函数正确执行
}
