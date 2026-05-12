#ifndef CONNECTIONPOOL_H
#define CONNECTIONPOOL_H

#include "db.h"
#include <memory>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <string>
using namespace std;

// 数据库连接池（单例）
// 启动时创建 poolSize 个 MySQL 长连接，所有 Model 通过 getConnection()
// 获取 shared_ptr<MySQL>，shared_ptr 析构时自动归还连接。
//
// 线程安全：getConnection() 在池空时阻塞等待，归还时唤醒。
// 连接健康：归还时 mysql_ping() 检测，断开的连接自动重建。
class ConnectionPool
{
public:
    static ConnectionPool* instance();

    // 初始化连接池：创建 poolSize 个已连接的 MySQL 对象
    void init(const string& host, int port,
              const string& user, const string& password,
              const string& dbname, int poolSize);

    // 取一个空闲连接，队列空时阻塞等待
    // 返回 shared_ptr<MySQL>，引用计数归零时自动归还到池
    shared_ptr<MySQL> getConnection();

private:
    ConnectionPool() = default;
    ConnectionPool(const ConnectionPool&) = delete;
    ConnectionPool& operator=(const ConnectionPool&) = delete;

    queue<MySQL*> _pool;
    mutex _mutex;
    condition_variable _cv;
    int _poolSize = 0;
    bool _initialized = false;

    // 保存连接参数，用于归还时发现连接断开后重建
    string _host;
    int _port = 0;
    string _user;
    string _password;
    string _dbname;
};

#endif
