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
        _configMap["server.node_id"] = js["server"].at("node_id").get<string>();

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
        const auto flow = js.value("flow", json::object());
        _configMap["flow.db_worker_threads"] = to_string(flow.value("db_worker_threads", js["server"]["thread_num"].get<int>()));
        _configMap["flow.db_max_queue_size"] = to_string(flow.value("db_max_queue_size", 2048));
        _configMap["flow.offline_claim_batch"] = to_string(flow.value("offline_claim_batch", 32));
        _configMap["flow.offline_users_per_round"] = to_string(flow.value("offline_users_per_round", 4));
        _configMap["flow.output_high_watermark_bytes"] = to_string(flow.value("output_high_watermark_bytes", 4 * 1024 * 1024));
        _configMap["flow.output_hard_limit_bytes"] = to_string(flow.value("output_hard_limit_bytes", 8 * 1024 * 1024));
        _configMap["flow.stream_soft_limit"] = to_string(flow.value("stream_soft_limit", 8000));
        if (getDbWorkerThreads() < 1 || getDbWorkerThreads() > 64 ||
            getDbMaxQueueSize() < 4 || getDbMaxQueueSize() > 65536 ||
            getOfflineClaimBatch() < 1 || getOfflineClaimBatch() > 128 ||
            getOfflineUsersPerRound() < 1 || getOfflineUsersPerRound() > 16 ||
            getOutputHighWatermarkBytes() < 1024 ||
            getOutputHardLimitBytes() < 2LL * getOutputHighWatermarkBytes() ||
            getOutputHardLimitBytes() > 64 * 1024 * 1024 ||
            getStreamSoftLimit() < 1 || getStreamSoftLimit() > 10000)
            throw std::invalid_argument("invalid flow control configuration");
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

string Config::getNodeId() const
{
    return _configMap.at("server.node_id");
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

int Config::getDbWorkerThreads() const { return stoi(_configMap.at("flow.db_worker_threads")); }
int Config::getDbMaxQueueSize() const { return stoi(_configMap.at("flow.db_max_queue_size")); }
int Config::getOfflineClaimBatch() const { return stoi(_configMap.at("flow.offline_claim_batch")); }
int Config::getOfflineUsersPerRound() const { return stoi(_configMap.at("flow.offline_users_per_round")); }
int Config::getOutputHighWatermarkBytes() const { return stoi(_configMap.at("flow.output_high_watermark_bytes")); }
int Config::getOutputHardLimitBytes() const { return stoi(_configMap.at("flow.output_hard_limit_bytes")); }
int Config::getStreamSoftLimit() const { return stoi(_configMap.at("flow.stream_soft_limit")); }
