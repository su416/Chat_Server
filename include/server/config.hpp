#ifndef CONFIG_HPP
#define CONFIG_HPP
#include <cstdint>
#include <string>
#include <unordered_map>
using namespace std;

// 简单配置模块：读取 conf/chatserver.json 中的配置
class Config
{
public:
    // 单例接口
    static Config& instance();

    // 加载配置文件
    bool load(const string& filename);

    // server
    string getServerIp() const;
    uint16_t getServerPort() const;
    int getThreadNum() const;
    string getServerName() const;
    string getNodeId() const;

    // mysql
    string getMySQLHost() const;
    uint16_t getMySQLPort() const;
    string getMySQLUser() const;
    string getMySQLPassword() const;
    string getMySQLDBName() const;
    string getMySQLCharset() const;

    // redis
    string getRedisHost() const;
    uint16_t getRedisPort() const;

    // business
    int getMaxHistoryCount() const;

     // protocol
    bool useProtobufHistory() const;

    // rate_limit
    bool isRateLimitEnabled() const;
    int getRateLimitCapacity() const;
    double getRateLimitPerSec() const;

private:
    Config() = default;
    Config(const Config&) = delete;
    Config& operator=(const Config&) = delete;

private:
    // 配置项缓存
    unordered_map<string, string> _configMap;
};

#endif
