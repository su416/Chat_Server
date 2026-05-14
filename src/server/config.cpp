#include "config.hpp"
#include "json.hpp"
#include <fstream>
#include <iostream>

using json = nlohmann::json;

// 单例对象
Config& Config::instance()
{
    static Config config;
    return config;
}

// 加载配置文件
bool Config::load(const string& filename)
{
    ifstream ifs(filename);
    if (!ifs.is_open())
    {
        cerr << "open config file failed: " << filename << endl;
        return false;
    }

    try
    {
        json js;
        ifs >> js;

        // server
        _configMap["server.ip"] = js["server"]["ip"];
        _configMap["server.port"] = to_string(js["server"]["port"].get<int>());
        _configMap["server.thread_num"] = to_string(js["server"]["thread_num"].get<int>());
        _configMap["server.name"] = js["server"]["name"];

        // mysql
        _configMap["mysql.host"] = js["mysql"]["host"];
        _configMap["mysql.port"] = to_string(js["mysql"]["port"].get<int>());
        _configMap["mysql.user"] = js["mysql"]["user"];
        _configMap["mysql.password"] = js["mysql"]["password"];
        _configMap["mysql.dbname"] = js["mysql"]["dbname"];
        _configMap["mysql.charset"] = js["mysql"]["charset"];

        // redis
        _configMap["redis.host"] = js["redis"]["host"];
        _configMap["redis.port"] = to_string(js["redis"]["port"].get<int>());

        // protocol
        _configMap["protocol.use_protobuf_history"] =
            js["protocol"]["use_protobuf_history"].get<bool>() ? "true" : "false";

        // business
        _configMap["business.max_history_count"] = to_string(js["business"]["max_history_count"].get<int>());

        // rate_limit（可选，不存在时使用默认值）
        if (js.contains("rate_limit"))
        {
            auto& rl = js["rate_limit"];
            _configMap["rate_limit.enabled"] = rl.value("enabled", true) ? "true" : "false";
            _configMap["rate_limit.capacity"] = to_string(rl.value("capacity", 1000));
            _configMap["rate_limit.rate_per_sec"] = to_string(rl.value("rate_per_sec", 10000.0));
        }
    }
    catch (const exception& e)
    {
        cerr << "parse config file failed: " << e.what() << endl;
        return false;
    }

    return true;
}

// server
string Config::getServerIp() const
{
    return _configMap.at("server.ip");
}

uint16_t Config::getServerPort() const
{
    return static_cast<uint16_t>(stoi(_configMap.at("server.port")));
}

int Config::getThreadNum() const
{
    return stoi(_configMap.at("server.thread_num"));
}

string Config::getServerName() const
{
    return _configMap.at("server.name");
}

// mysql
string Config::getMySQLHost() const
{
    return _configMap.at("mysql.host");
}

uint16_t Config::getMySQLPort() const
{
    return static_cast<uint16_t>(stoi(_configMap.at("mysql.port")));
}

string Config::getMySQLUser() const
{
    return _configMap.at("mysql.user");
}

string Config::getMySQLPassword() const
{
    return _configMap.at("mysql.password");
}

string Config::getMySQLDBName() const
{
    return _configMap.at("mysql.dbname");
}

string Config::getMySQLCharset() const
{
    return _configMap.at("mysql.charset");
}

// redis
string Config::getRedisHost() const
{
    return _configMap.at("redis.host");
}

uint16_t Config::getRedisPort() const
{
    return static_cast<uint16_t>(stoi(_configMap.at("redis.port")));
}

// business
int Config::getMaxHistoryCount() const
{
    return stoi(_configMap.at("business.max_history_count"));
}

 // protocol
bool Config::useProtobufHistory() const
{
    auto it = _configMap.find("protocol.use_protobuf_history");
    if (it == _configMap.end())
    {
        return false;
    }
    return it->second == "true";
}
// rate_limit
bool Config::isRateLimitEnabled() const
{
    auto it = _configMap.find("rate_limit.enabled");
    if (it == _configMap.end()) return true;
    return it->second == "true";
}

int Config::getRateLimitCapacity() const
{
    auto it = _configMap.find("rate_limit.capacity");
    if (it == _configMap.end()) return 1000;
    return stoi(it->second);
}

double Config::getRateLimitPerSec() const
{
    auto it = _configMap.find("rate_limit.rate_per_sec");
    if (it == _configMap.end()) return 10000.0;
    return stod(it->second);
}
