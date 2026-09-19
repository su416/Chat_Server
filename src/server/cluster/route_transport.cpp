#include "cluster/route_transport.hpp"
#include "config.hpp"
#include <muduo/base/Logging.h>
#include <chrono>
#include <thread>
#include <algorithm>
#include <cstdlib>

namespace cluster {
namespace {
constexpr int maxAttempts = 5;
constexpr int maxEntries = 10000;
constexpr int maxPayloadBytes = 65536;

// Redis 7 XTRIM does not protect PEL entries. Trim only the acknowledged prefix.
const char* safeTrim = R"lua(
local pending = redis.call('XPENDING', KEYS[1], ARGV[1])
local boundary
if pending[1] > 0 then
  boundary = pending[2]
else
  local groups = redis.call('XINFO', 'GROUPS', KEYS[1])
  for _, group in ipairs(groups) do
    local name, last
    for i = 1, #group, 2 do
      if group[i] == 'name' then name = group[i+1] end
      if group[i] == 'last-delivered-id' then last = group[i+1] end
    end
    if name == ARGV[1] then boundary = last end
  end
end
if boundary and boundary ~= '0-0' then
  return redis.call('XTRIM', KEYS[1], 'MINID', boundary)
end
return 0
)lua";

const char* dedupScript = R"lua(
local now = redis.call('TIME')
local seconds = tonumber(now[1])
local score = redis.call('ZSCORE', KEYS[1], ARGV[1])
if score and tonumber(score) > seconds then return 0 end
redis.call('ZREMRANGEBYSCORE', KEYS[1], '-inf', seconds)
if redis.call('ZCARD', KEYS[1]) >= 100000 then return -1 end
redis.call('ZADD', KEYS[1], seconds + 86400, ARGV[1])
redis.call('EXPIRE', KEYS[1], 86400)
return 1
)lua";

bool isString(const redisReply* reply) {
    return reply && (reply->type == REDIS_REPLY_STRING ||
                     reply->type == REDIS_REPLY_STATUS) && reply->str;
}
std::string asString(const redisReply* reply) {
    return reply && reply->str ? std::string(reply->str, reply->len) : std::string{};
}
std::vector<std::pair<std::string, std::string>> parseEntries(const redisReply* entries) {
    std::vector<std::pair<std::string, std::string>> result;
    if (!entries || entries->type != REDIS_REPLY_ARRAY) return result;
    for (size_t i = 0; i < entries->elements; ++i) {
        const auto* entry = entries->element[i];
        if (!entry || entry->type != REDIS_REPLY_ARRAY || entry->elements != 2 ||
            !isString(entry->element[0])) continue;
        const auto* fields = entry->element[1];
        if (!fields || fields->type != REDIS_REPLY_ARRAY) continue;
        for (size_t field = 0; field + 1 < fields->elements; field += 2) {
            if (asString(fields->element[field]) == "body" &&
                isString(fields->element[field + 1])) {
                result.emplace_back(asString(entry->element[0]),
                                    asString(fields->element[field + 1]));
                break;
            }
        }
    }
    return result;
}
RedisContextPtr consumerContext() {
    const auto& config = Config::instance();
    timeval timeout{1, 0};
    RedisContextPtr context(redisConnectWithTimeout(
        config.getRedisHost().c_str(), config.getRedisPort(), timeout));
    if (context && !context->err) redisSetTimeout(context.get(), timeval{3, 0});
    return context;
}
RedisReplyPtr direct(redisContext* context, const std::vector<std::string>& args) {
    std::vector<const char*> values;
    std::vector<size_t> lengths;
    for (const auto& arg : args) {
        values.push_back(arg.data());
        lengths.push_back(arg.size());
    }
    return RedisReplyPtr(static_cast<redisReply*>(redisCommandArgv(
        context, static_cast<int>(values.size()), values.data(), lengths.data())));
}
}

std::string RoutedMessage::encode() const {
    return nlohmann::json{{"version", 1}, {"route_id", routeId},
        {"source_node", sourceNode}, {"target_node", targetNode},
        {"message_type", messageType}, {"sender_user_id", senderUserId},
        {"receiver_user_id", receiverUserId}, {"hops", hops},
        {"payload", payload}}.dump();
}
std::optional<RoutedMessage> RoutedMessage::decode(const std::string& encoded) {
    try {
        auto parsed = nlohmann::json::parse(encoded);
        if (!parsed.is_object() || parsed.at("version") != 1 ||
            !parsed.at("payload").is_object()) return std::nullopt;
        const auto& body = parsed.at("payload");
        if (!body.contains("msgid") || !body.at("msgid").is_number_integer())
            return std::nullopt;
        RoutedMessage message;
        message.routeId = parsed.at("route_id").get<std::string>();
        message.sourceNode = parsed.at("source_node").get<std::string>();
        message.targetNode = parsed.at("target_node").get<std::string>();
        message.messageType = parsed.at("message_type").get<int>();
        message.senderUserId = parsed.at("sender_user_id").get<int>();
        message.receiverUserId = parsed.at("receiver_user_id").get<int>();
        message.hops = parsed.at("hops").get<int>();
        message.payload = parsed.at("payload");
        if (message.routeId.empty() || message.sourceNode.empty() ||
            message.targetNode.empty() || message.receiverUserId <= 0 ||
            message.hops < 0 || message.hops > 1 || message.messageType <= 0 ||
            body.at("msgid").get<int>() != message.messageType)
            return std::nullopt;
        return message;
    } catch (...) { return std::nullopt; }
}

RouteTransport::RouteTransport(Redis& redis, std::string nodeId, std::string generation)
    : _redis(redis), _nodeId(std::move(nodeId)), _generation(std::move(generation)),
      _group("chat:route:group:" + _nodeId),
      _consumer(_nodeId + ":" + _generation) {}
RouteTransport::~RouteTransport() { stop(); }
std::string RouteTransport::streamFor(const std::string& nodeId) {
    return "chat:route:stream:" + nodeId;
}
bool RouteTransport::start(std::function<RouteOutcome(RoutedMessage)> handler) {
    if (!handler || _worker.joinable()) return false;
    _handler = std::move(handler);
    _stopping = false;
    _worker = std::thread(&RouteTransport::consume, this);
    return true;
}
void RouteTransport::stop() {
    _stopping = true;
    if (_worker.joinable()) _worker.join();
}
bool RouteTransport::send(const RoutedMessage& message) {
    if (message.targetNode.empty() || message.hops > 1) return false;
    const auto encoded = message.encode();
    if (encoded.size() > maxPayloadBytes) { ++_failed; return false; }
    const auto key = streamFor(message.targetNode);
    // Fail closed at capacity: never evict unacknowledged route entries.
    auto reply = _redis.command({"EVAL", R"lua(
if redis.call('XLEN', KEYS[1]) >= tonumber(ARGV[1]) then return -1 end
local id = redis.call('XADD', KEYS[1], '*', 'body', ARGV[2])
return {id, redis.call('XLEN', KEYS[1])}
)lua", "1", key, std::to_string(std::min(maxEntries,
    Config::instance().getStreamSoftLimit())), encoded});
    if (reply && reply->type == REDIS_REPLY_INTEGER && reply->integer == -1) {
        const auto count = ++_overloadRejected;
        if (count == 1 || count % 100 == 0)
            LOG_WARN << "ROUTE_STREAM_OVERLOADED target=" << message.targetNode
                     << " count=" << count;
        return false;
    }
    if (!reply || reply->type != REDIS_REPLY_ARRAY || reply->elements != 2 ||
        !isString(reply->element[0]) || reply->element[1]->type != REDIS_REPLY_INTEGER) {
        ++_failed;
        LOG_ERROR << "ROUTE_STREAM_FAILED node=" << message.targetNode;
        return false;
    }
    const auto length = static_cast<std::uint64_t>(reply->element[1]->integer);
    auto peak = _targetStreamPeak.load();
    while (peak < length && !_targetStreamPeak.compare_exchange_weak(peak, length)) {}
    ++_produced;
    return true;
}
RouteTransport::DedupResult RouteTransport::markAccepted(const std::string& routeId) {
    auto reply = _redis.command({"EVAL", dedupScript, "1",
                                  "chat:route:dedup:" + _nodeId, routeId});
    if (!reply || reply->type != REDIS_REPLY_INTEGER) return DedupResult::Error;
    if (reply->integer == 0) { ++_duplicate; return DedupResult::Duplicate; }
    return reply->integer == 1 ? DedupResult::New : DedupResult::Error;
}
bool RouteTransport::ack(const std::string& entryId) {
    auto reply = _redis.command({"XACK", streamFor(_nodeId), _group, entryId});
    if (!reply || reply->type != REDIS_REPLY_INTEGER) return false;
    ++_acked;
    // Best-effort trimming; never roll back an ACK because trimming failed.
    _redis.command({"EVAL", safeTrim, "1", streamFor(_nodeId), _group});
    return true;
}
bool RouteTransport::handleEntry(const std::string& entryId,
                                 const std::string& payload, int attempts) {
    if (const char* delay = std::getenv("CHAT_PHASE8_TEST_ROUTE_CONSUMER_DELAY_MS")) {
        try {
            const auto ms = std::stoul(delay);
            if (ms <= 1000) std::this_thread::sleep_for(std::chrono::milliseconds(ms));
        } catch (...) { /* invalid test hook is ignored */ }
    }
    ++_consumed;
    auto message = RoutedMessage::decode(payload);
    if (!message) {
        LOG_ERROR << "ROUTE_INVALID stream_id=" << entryId;
        ++_failed;
        return ack(entryId);
    }
    auto outcome = _handler(std::move(*message));
    if (outcome == RouteOutcome::Accepted) return ack(entryId);
    if (outcome == RouteOutcome::Terminal) { ++_failed; return ack(entryId); }
    ++_retried;
    if (outcome == RouteOutcome::RetryInfrastructure) {
        LOG_ERROR << "ROUTE_INFRASTRUCTURE_RETRY stream_id=" << entryId
                  << " attempts=" << attempts;
        return false; // DB outage cannot acknowledge or dead-letter this route.
    }
    if (attempts >= maxAttempts) {
        auto parked = _redis.command({"XADD", "chat:route:dead:" + _nodeId,
            "MAXLEN", "~", "1000", "*", "stream_id", entryId, "body", payload});
        if (!isString(parked.get())) { ++_failed; return false; }
        LOG_ERROR << "ROUTE_TERMINAL stream_id=" << entryId << " attempts=" << attempts;
        ++_failed;
        return ack(entryId);
    }
    return false;
}
void RouteTransport::consume() {
    const auto key = streamFor(_nodeId);
    while (!_stopping) {
        auto context = consumerContext();
        if (!context || context->err) {
            ++_failed;
            std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
        }
        auto group = direct(context.get(),
                            {"XGROUP", "CREATE", key, _group, "0", "MKSTREAM"});
        if (!group || (group->type == REDIS_REPLY_ERROR &&
            asString(group.get()).find("BUSYGROUP") == std::string::npos)) {
            ++_failed;
            LOG_ERROR << "ROUTE_GROUP_CREATE failed node=" << _nodeId;
            std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
        }
        std::string claimCursor = "0-0";
        auto nextSample = std::chrono::steady_clock::now();
        while (!_stopping && !context->err) {
            if (std::chrono::steady_clock::now() >= nextSample) {
                nextSample = std::chrono::steady_clock::now() + std::chrono::seconds(2);
                auto length = direct(context.get(), {"XLEN", key});
                if (length && length->type == REDIS_REPLY_INTEGER) {
                    const auto value = static_cast<std::uint64_t>(length->integer);
                    _streamLength = value;
                    auto peak = _streamPeak.load();
                    while (peak < value && !_streamPeak.compare_exchange_weak(peak, value)) {}
                }
                auto pending = direct(context.get(), {"XPENDING", key, _group});
                if (pending && pending->type == REDIS_REPLY_ARRAY &&
                    pending->elements >= 2 && pending->element[0]->type == REDIS_REPLY_INTEGER) {
                    _pending = static_cast<std::uint64_t>(pending->element[0]->integer);
                    std::uint64_t oldest = 0;
                    if (_pending && isString(pending->element[1])) {
                        try {
                            const auto id = asString(pending->element[1]);
                            const auto created = std::stoull(id.substr(0, id.find('-')));
                            const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::system_clock::now().time_since_epoch()).count();
                            if (created < static_cast<std::uint64_t>(now))
                                oldest = static_cast<std::uint64_t>(now) - created;
                        } catch (...) { /* metric remains zero */ }
                    }
                    _oldestPendingMs = oldest;
                }
                auto groups = direct(context.get(), {"XINFO", "GROUPS", key});
                if (groups && groups->type == REDIS_REPLY_ARRAY) {
                    for (std::size_t g = 0; g < groups->elements; ++g) {
                        const auto* fields = groups->element[g];
                        if (!fields || fields->type != REDIS_REPLY_ARRAY) continue;
                        bool selected = false;
                        std::uint64_t lag = 0;
                        for (std::size_t i = 0; i + 1 < fields->elements; i += 2) {
                            const auto name = asString(fields->element[i]);
                            if (name == "name") selected = asString(fields->element[i + 1]) == _group;
                            if (name == "lag" && fields->element[i + 1]->type == REDIS_REPLY_INTEGER)
                                lag = static_cast<std::uint64_t>(fields->element[i + 1]->integer);
                        }
                        if (selected) { _lag = lag; break; }
                    }
                }
            }
            auto claimed = _redis.command({"XAUTOCLAIM", key, _group, _consumer,
                                           "2000", claimCursor, "COUNT", "8"});
            if (claimed && claimed->type == REDIS_REPLY_ARRAY && claimed->elements >= 2) {
                claimCursor = asString(claimed->element[0]);
                if (claimCursor.empty()) claimCursor = "0-0";
                auto entries = parseEntries(claimed->element[1]);
                _claimed += entries.size();
                for (const auto& [id, body] : entries) {
                    auto pending = _redis.command({"XPENDING", key, _group, id, id, "1"});
                    int attempts = 1;
                    if (pending && pending->type == REDIS_REPLY_ARRAY && pending->elements &&
                        pending->element[0]->type == REDIS_REPLY_ARRAY &&
                        pending->element[0]->elements >= 4 &&
                        pending->element[0]->element[3]->type == REDIS_REPLY_INTEGER)
                        attempts = static_cast<int>(pending->element[0]->element[3]->integer);
                    handleEntry(id, body, attempts);
                }
            } else claimCursor = "0-0";
            auto reply = direct(context.get(), {"XREADGROUP", "GROUP", _group, _consumer,
                "COUNT", "8", "BLOCK", "1000", "STREAMS", key, ">"});
            if (!reply) break;
            if (reply->type == REDIS_REPLY_ARRAY && reply->elements > 0 &&
                reply->element[0]->type == REDIS_REPLY_ARRAY &&
                reply->element[0]->elements == 2) {
                for (const auto& [id, body] : parseEntries(reply->element[0]->element[1]))
                    handleEntry(id, body, 1);
            }
        }
        if (!_stopping) std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}
RouteMetrics RouteTransport::metrics(bool fresh) const {
    RouteMetrics result;
    result.produced = _produced;
    result.consumed = _consumed;
    result.acked = _acked;
    result.retried = _retried;
    result.claimed = _claimed;
    result.duplicate = _duplicate;
    result.failed = _failed;
    result.pending = _pending;
    result.streamLength = _streamLength;
    result.streamPeak = _streamPeak;
    result.oldestPendingMs = _oldestPendingMs;
    result.lag = _lag;
    result.overloadRejected = _overloadRejected;
    result.targetStreamPeak = _targetStreamPeak;
    if (fresh) {
        auto pending = _redis.command({"XPENDING", streamFor(_nodeId), _group});
        if (pending && pending->type == REDIS_REPLY_ARRAY && pending->elements &&
            pending->element[0]->type == REDIS_REPLY_INTEGER)
            result.pending = static_cast<std::uint64_t>(pending->element[0]->integer);
    }
    return result;
}
std::string RouteTransport::nextRouteId() {
    const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    return _nodeId + ":" + _generation + ":" + std::to_string(millis) + ":" +
           std::to_string(_next.fetch_add(1));
}
} // namespace cluster
