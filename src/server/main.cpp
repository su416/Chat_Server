#include "chatserver.hpp"
#include "chatservice.hpp"
#include "config.hpp"
#include "connectionpool.h"
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
