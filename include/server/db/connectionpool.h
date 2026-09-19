#ifndef CONNECTIONPOOL_H
#define CONNECTIONPOOL_H

#include "db.h"
#include <memory>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <string>
#include <atomic>
#include <cstdint>
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

    // 取连接，timeoutMs 毫秒内无可用连接返回 nullptr（降级用）
    shared_ptr<MySQL> getConnection(int timeoutMs);

    // 当前空闲连接数
    int idleCount();
    std::uint64_t totalWaitMs() const { return _totalWaitMs.load(); }
    std::uint64_t waitCount() const { return _waitCount.load(); }

    // 销毁池中所有连接，进程退出前调用
    void shutdown();

private:
    ConnectionPool() = default;
    ~ConnectionPool();
    ConnectionPool(const ConnectionPool&) = delete;
    ConnectionPool& operator=(const ConnectionPool&) = delete;

    // 用自定义 deleter 包装连接：析构时归还到池
    shared_ptr<MySQL> wrapConnection(MySQL* conn);

    queue<MySQL*> _pool;
    mutex _mutex;
    condition_variable _cv;
    int _poolSize = 0;
    bool _initialized = false;
    std::atomic<std::uint64_t> _totalWaitMs{0}, _waitCount{0};

    // 保存连接参数，用于归还时发现连接断开后重建
    string _host;
    int _port = 0;
    string _user;
    string _password;
    string _dbname;
};

#endif
