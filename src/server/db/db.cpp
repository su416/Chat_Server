#include "db.h"
#include "config.hpp"
#include <muduo/base/Logging.h>
#include <cstring>

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

// 连接数据库（从 Config 读取凭据）
bool MySQL::connect()
{
    auto& cfg = Config::instance();
    return connect(cfg.getMySQLHost(), cfg.getMySQLPort(),
                   cfg.getMySQLUser(), cfg.getMySQLPassword(),
                   cfg.getMySQLDBName());
}

// 连接数据库（参数化）
bool MySQL::connect(const string& host, int port,
                    const string& user, const string& password,
                    const string& dbname)
{
    MYSQL *p = mysql_real_connect(_conn, host.c_str(), user.c_str(),
                                  password.c_str(), dbname.c_str(),
                                  port, nullptr, 0);
    if (p != nullptr)
    {
        auto& cfg = Config::instance();
        string charset = cfg.getMySQLCharset();
        string setNames = "set names " + charset;
        mysql_query(_conn, setNames.c_str());
        LOG_INFO << "connect mysql success!";
    }
    else
    {
        LOG_INFO << "connect mysql fail!";
    }

    return p;
}

// 检查连接是否存活
bool MySQL::ping()
{
    return mysql_ping(_conn) == 0;
}

// 更新操作
bool MySQL::update(string sql)
{
    if (mysql_query(_conn, sql.c_str()))
    {
        LOG_INFO << __FILE__ << ":" << __LINE__ << ": "
                 << sql << " update failed!";
        return false;
    }

    return true;
}

// 查询操作
MYSQL_RES *MySQL::query(string sql)
{
    if (mysql_query(_conn, sql.c_str()))
    {
        LOG_INFO << __FILE__ << ":" << __LINE__ << ": "
                 << sql << " query failed!";
        return nullptr;
    }

    return mysql_use_result(_conn);
}

// 新增：prepared statement 执行
bool MySQL::executePrepared(const string &sql, MYSQL_BIND *bind, int paramCount)
{
    MYSQL_STMT *stmt = mysql_stmt_init(_conn);
    if (stmt == nullptr)
    {
        LOG_ERROR << "mysql_stmt_init failed";
        return false;
    }

    if (mysql_stmt_prepare(stmt, sql.c_str(), sql.size()) != 0)
    {
        LOG_ERROR << "mysql_stmt_prepare failed: " << mysql_stmt_error(stmt);
        mysql_stmt_close(stmt);
        return false;
    }

    if (static_cast<int>(mysql_stmt_param_count(stmt)) != paramCount)
    {
        LOG_ERROR << "mysql_stmt_param_count mismatch, expect="
                  << paramCount << " actual=" << mysql_stmt_param_count(stmt);
        mysql_stmt_close(stmt);
        return false;
    }

    if (mysql_stmt_bind_param(stmt, bind) != 0)
    {
        LOG_ERROR << "mysql_stmt_bind_param failed: " << mysql_stmt_error(stmt);
        mysql_stmt_close(stmt);
        return false;
    }

    if (mysql_stmt_execute(stmt) != 0)
    {
        LOG_ERROR << "mysql_stmt_execute failed: " << mysql_stmt_error(stmt);
        mysql_stmt_close(stmt);
        return false;
    }

    mysql_stmt_close(stmt);
    return true;
}

// 获取连接
MYSQL* MySQL::getConnection()
{
    return _conn;
}