#ifndef DB_H
#define DB_H

#include <mysql/mysql.h>
#include <string>
using namespace std;

// 数据库操作类
class MySQL
{
public:
    // 初始化数据库连接
    MySQL();

    // 释放数据库连接资源
    ~MySQL();

    // 连接数据库（从 Config 读取凭据，供连接池 init 前临时使用）
    bool connect();

    // 连接数据库（参数化，供连接池使用）
    bool connect(const string& host, int port,
                 const string& user, const string& password,
                 const string& dbname);

    // 更新操作
    bool update(string sql);

    // 查询操作
    MYSQL_RES *query(string sql);

    // 执行 prepared statement
    bool executePrepared(const string &sql, MYSQL_BIND *bind, int paramCount);

    // 获取原始连接句柄
    MYSQL* getConnection();

    // 检查连接是否存活（mysql_ping）
    bool ping();

private:
    MYSQL *_conn;
};

#endif