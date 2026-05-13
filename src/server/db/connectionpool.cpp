#include "connectionpool.h"
#include <muduo/base/Logging.h>

ConnectionPool* ConnectionPool::instance()
{
    static ConnectionPool pool;
    return &pool;
}

void ConnectionPool::init(const string& host, int port,
                          const string& user, const string& password,
                          const string& dbname, int poolSize)
{
    if (_initialized)
        return;

    _host = host;
    _port = port;
    _user = user;
    _password = password;
    _dbname = dbname;
    _poolSize = poolSize;

    for (int i = 0; i < poolSize; ++i)
    {
        MySQL* conn = new MySQL();
        if (conn->connect(host, port, user, password, dbname))
        {
            _pool.push(conn);
            LOG_INFO << "ConnectionPool: created connection " << (i + 1)
                     << "/" << poolSize;
        }
        else
        {
            LOG_ERROR << "ConnectionPool: failed to create connection "
                      << (i + 1);
            delete conn;
        }
    }

    _initialized = true;
    LOG_INFO << "ConnectionPool initialized with " << _pool.size()
             << " connections";
}

shared_ptr<MySQL> ConnectionPool::getConnection()
{
    unique_lock<mutex> lock(_mutex);

    // 池空则阻塞，等待其他线程归还连接
    _cv.wait(lock, [this] { return !_pool.empty(); });

    MySQL* conn = _pool.front();
    _pool.pop();

    // 自定义 deleter：shared_ptr 析构时归还连接而非 delete
    return shared_ptr<MySQL>(conn, [this](MySQL* c) {
        if (c->ping())
        {
            // 连接正常，放回池中
            {
                lock_guard<mutex> lg(_mutex);
                _pool.push(c);
            }
            _cv.notify_one();
        }
        else
        {
            // 连接已断开（MySQL wait_timeout 等），重建一条
            LOG_INFO << "ConnectionPool: stale connection, recreating...";
            MySQL* newConn = new MySQL();
            if (newConn->connect(_host, _port, _user, _password, _dbname))
            {
                {
                    lock_guard<mutex> lg(_mutex);
                    _pool.push(newConn);
                }
                _cv.notify_one();
            }
            else
            {
                LOG_ERROR << "ConnectionPool: failed to recreate connection";
                delete newConn;
            }
            // 旧连接已断，直接 delete
            delete c;
        }
    });
}

ConnectionPool::~ConnectionPool()
{
    shutdown();
}

void ConnectionPool::shutdown()
{
    lock_guard<mutex> lg(_mutex);
    while (!_pool.empty())
    {
        delete _pool.front();
        _pool.pop();
    }
    LOG_INFO << "ConnectionPool: shutdown, released all connections";
}
