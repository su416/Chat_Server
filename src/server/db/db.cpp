#include "db.h"
#include "config.hpp"
#include <muduo/base/Logging.h>
#include <string>

// 初始化数据库连接
MySQL::MySQL()
{
    _conn = mysql_init(nullptr);
}

// 释放数据库连接资源
MySQL::~MySQL()
{
    if (_conn != nullptr)
        mysql_close(_conn);
}

// 连接数据库
bool MySQL::connect()
{
    string host = Config::instance().getMySQLHost();
    uint16_t port = Config::instance().getMySQLPort();
    string user = Config::instance().getMySQLUser();
    string password = Config::instance().getMySQLPassword();
    string dbname = Config::instance().getMySQLDBName();
    string charset = Config::instance().getMySQLCharset();

    MYSQL *p = mysql_real_connect(_conn,
                                  host.c_str(),
                                  user.c_str(),
                                  password.c_str(),
                                  dbname.c_str(),
                                  port,
                                  nullptr,
                                  0);

    if (p != nullptr)
    {
        string sql = "set names " + charset;
        mysql_query(_conn, sql.c_str());
        LOG_INFO << "connect mysql success!";
    }
    else
    {
        LOG_INFO << "connect mysql fail!";
    }

    return p;
}

// 更新操作
bool MySQL::update(string sql)
{
    if (mysql_query(_conn, sql.c_str()))
    {
        LOG_INFO << __FILE__ << ":" << __LINE__ << ":"
                 << sql << "更新失败!";
        return false;
    }

    return true;
}

// 查询操作
MYSQL_RES *MySQL::query(string sql)
{
    if (mysql_query(_conn, sql.c_str()))
    {
        LOG_INFO << __FILE__ << ":" << __LINE__ << ":"
                 << sql << "查询失败!";
        return nullptr;
    }

    return mysql_use_result(_conn);
}

// 获取连接
MYSQL* MySQL::getConnection()
{
    return _conn;
}
