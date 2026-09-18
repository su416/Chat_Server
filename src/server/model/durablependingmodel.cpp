#include "model/durablependingmodel.hpp"
#include "connectionpool.h"

#include <muduo/base/Logging.h>
#include <mysql/mysql.h>

#include <chrono>
#include <algorithm>
#include <limits>
#include <memory>
#include <string>

namespace gateway {
namespace {
constexpr std::uint64_t kRetentionMs = 24ULL * 60 * 60 * 1000;
constexpr std::uint64_t kAckTimeoutMs = 2000;

std::shared_ptr<MySQL> borrow() {
    // Never block a DB worker indefinitely when the pool is depleted.
    return ConnectionPool::instance()->getConnection(500);
}
std::string quoted(MYSQL* conn, const std::string& value) {
    std::string escaped(value.size() * 2 + 1, '\0');
    const auto size = mysql_real_escape_string(conn, escaped.data(),
                                               value.data(), value.size());
    escaped.resize(size);
    return "'" + escaped + "'";
}
bool execute(MYSQL* conn, const std::string& sql) {
    if (mysql_query(conn, sql.c_str()) == 0) return true;
    LOG_ERROR << "DURABLE_PENDING_DB query failed code=" << mysql_errno(conn)
              << " error=" << mysql_error(conn);
    return false;
}
std::string field(MYSQL_ROW row, unsigned long* lengths, int index) {
    return row[index] ? std::string(row[index], lengths[index]) : std::string{};
}
std::optional<DurablePendingRecord> parseRow(MYSQL_ROW row, unsigned long* lengths) {
    try {
        DurablePendingRecord record;
        record.deliveryId = std::stoull(field(row, lengths, 0));
        record.routeId = field(row, lengths, 1);
        record.ownerNode = field(row, lengths, 2);
        record.receiverUserId = std::stoi(field(row, lengths, 3));
        record.messageType = std::stoi(field(row, lengths, 4));
        record.payload = field(row, lengths, 5);
        record.retryCount = std::stoul(field(row, lengths, 6));
        record.nextRetryAtMs = std::stoull(field(row, lengths, 7));
        record.expireAtMs = std::stoull(field(row, lengths, 8));
        const auto state = std::stoi(field(row, lengths, 9));
        if (record.deliveryId == 0 || record.receiverUserId <= 0 ||
            state < 0 || state > 3) return std::nullopt;
        record.state = static_cast<DurableState>(state);
        return record;
    } catch (...) { return std::nullopt; }
}
const char* columns =
    "delivery_id,route_id,owner_node,receiver_user_id,message_type,payload,"
    "retry_count,next_retry_at_ms,expire_at_ms,state";
std::optional<std::vector<DurablePendingRecord>> queryRows(
    MYSQL* conn, const std::string& sql) {
    if (!execute(conn, sql)) return std::nullopt;
    std::unique_ptr<MYSQL_RES, decltype(&mysql_free_result)> result(
        mysql_store_result(conn), &mysql_free_result);
    if (!result) {
        LOG_ERROR << "DURABLE_PENDING_DB store_result failed code=" << mysql_errno(conn);
        return std::nullopt;
    }
    std::vector<DurablePendingRecord> records;
    while (auto row = mysql_fetch_row(result.get())) {
        auto parsed = parseRow(row, mysql_fetch_lengths(result.get()));
        if (!parsed) return std::nullopt;
        records.push_back(std::move(*parsed));
    }
    return records;
}
}

std::uint64_t unixMillis() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

PersistResult DurablePendingModel::persist(
    const std::string& routeId, const std::string& nodeId, int receiverUserId,
    int messageType, const std::string& payload, std::uint64_t nowMs) {
    if (nodeId.empty() || nodeId.size() > 64 || routeId.size() > 192 ||
        receiverUserId <= 0 || messageType <= 0 || payload.empty() ||
        payload.size() > 1024 * 1024 || nowMs > std::numeric_limits<std::uint64_t>::max() - kRetentionMs)
        return {};
    auto connection = borrow();
    if (!connection) return {};
    auto* conn = connection->getConnection();
    const auto sql = "INSERT INTO pending_delivery"
        "(route_id,owner_node,receiver_user_id,message_type,payload,retry_count,"
        "next_retry_at_ms,expire_at_ms,state,created_at_ms,updated_at_ms) VALUES(" +
        (routeId.empty() ? std::string("NULL") : quoted(conn, routeId)) + "," +
        quoted(conn, nodeId) + "," + std::to_string(receiverUserId) + "," +
        std::to_string(messageType) + "," + quoted(conn, payload) + ",0," +
        std::to_string(nowMs + kAckTimeoutMs) + "," +
        std::to_string(nowMs + kRetentionMs) + ",0," +
        std::to_string(nowMs) + "," + std::to_string(nowMs) + ")";
    if (mysql_query(conn, sql.c_str()) == 0) {
        DurablePendingRecord row;
        row.deliveryId = mysql_insert_id(conn);
        row.routeId = routeId;
        row.ownerNode = nodeId;
        row.receiverUserId = receiverUserId;
        row.messageType = messageType;
        row.payload = payload;
        row.nextRetryAtMs = nowMs + kAckTimeoutMs;
        row.expireAtMs = nowMs + kRetentionMs;
        return {PersistStatus::Inserted, std::move(row)};
    }
    if (mysql_errno(conn) != 1062 || routeId.empty()) {
        LOG_ERROR << "DURABLE_PENDING_DB insert failed code=" << mysql_errno(conn);
        return {};
    }
    auto existing = queryRows(conn, "SELECT " + std::string(columns) +
        " FROM pending_delivery WHERE route_id=" + quoted(conn, routeId) +
        " LIMIT 1");
    if (!existing || existing->size() != 1 ||
        existing->front().receiverUserId != receiverUserId ||
        existing->front().messageType != messageType ||
        existing->front().payload != payload) return {};
    if (existing->front().state == DurableState::Pending ||
        existing->front().state == DurableState::WaitingSession) {
        if (existing->front().ownerNode != nodeId) {
            if (!execute(conn, "UPDATE pending_delivery SET owner_node=" +
                quoted(conn, nodeId) + ",state=0,next_retry_at_ms=" +
                std::to_string(nowMs) + ",updated_at_ms=" + std::to_string(nowMs) +
                " WHERE route_id=" + quoted(conn, routeId) +
                " AND state IN(0,1)")) return {};
            existing->front().ownerNode = nodeId;
            existing->front().state = DurableState::Pending;
            existing->front().nextRetryAtMs = nowMs;
        }
    }
    return {PersistStatus::Existing, std::move(existing->front())};
}

DurableAckStatus DurablePendingModel::markAcked(
    std::uint64_t id, int user, std::uint64_t nowMs) {
    auto connection = borrow();
    if (!connection || id == 0 || user <= 0) return DurableAckStatus::Error;
    auto* conn = connection->getConnection();
    if (!execute(conn,
        "UPDATE pending_delivery SET state=2,updated_at_ms=" + std::to_string(nowMs) +
        " WHERE delivery_id=" + std::to_string(id) +
        " AND receiver_user_id=" + std::to_string(user) + " AND state IN(0,1)"))
        return DurableAckStatus::Error;
    if (mysql_affected_rows(conn) == 1) return DurableAckStatus::Completed;
    auto rows = queryRows(conn, "SELECT " + std::string(columns) +
        " FROM pending_delivery WHERE delivery_id=" + std::to_string(id) +
        " AND receiver_user_id=" + std::to_string(user) + " LIMIT 1");
    if (rows && rows->size() == 1 &&
        rows->front().state == DurableState::Acked)
        return DurableAckStatus::AlreadyCompleted;
    return DurableAckStatus::Error;
}
bool DurablePendingModel::markWaiting(
    std::uint64_t id, const std::string& node,
    std::uint64_t expectedNext, std::uint64_t nowMs) {
    auto connection = borrow();
    if (!connection || id == 0 || node.empty()) return false;
    auto* conn = connection->getConnection();
    return execute(conn,
        "UPDATE pending_delivery SET state=1,updated_at_ms=" + std::to_string(nowMs) +
        " WHERE delivery_id=" + std::to_string(id) + " AND owner_node=" +
        quoted(conn, node) + " AND state=0 AND next_retry_at_ms=" +
        std::to_string(expectedNext));
}
bool DurablePendingModel::markWaitingForUser(
    int user, const std::string& node, std::uint64_t nowMs) {
    auto connection = borrow();
    if (!connection || user <= 0 || node.empty() || node.size() > 64) return false;
    auto* conn = connection->getConnection();
    return execute(conn, "UPDATE pending_delivery SET state=1,updated_at_ms=" +
        std::to_string(nowMs) + " WHERE receiver_user_id=" + std::to_string(user) +
        " AND owner_node=" + quoted(conn, node) + " AND state=0");
}
bool DurablePendingModel::transferOwner(
    std::uint64_t id, const std::string& expectedOwner,
    const std::string& newOwner, std::uint64_t nowMs) {
    auto connection = borrow();
    if (!connection || id == 0 || expectedOwner.empty() || newOwner.empty() ||
        expectedOwner.size() > 64 || newOwner.size() > 64) return false;
    auto* conn = connection->getConnection();
    return execute(conn, "UPDATE pending_delivery SET owner_node=" +
        quoted(conn, newOwner) + ",state=0,next_retry_at_ms=" +
        std::to_string(nowMs) + ",updated_at_ms=" + std::to_string(nowMs) +
        " WHERE delivery_id=" + std::to_string(id) + " AND owner_node=" +
        quoted(conn, expectedOwner) + " AND state IN(0,1) AND expire_at_ms>" +
        std::to_string(nowMs));
}
bool DurablePendingModel::markExpired(std::uint64_t id, std::uint64_t nowMs) {
    auto connection = borrow();
    if (!connection || id == 0) return false;
    return execute(connection->getConnection(),
        "UPDATE pending_delivery SET state=3,updated_at_ms=" + std::to_string(nowMs) +
        " WHERE delivery_id=" + std::to_string(id) + " AND state IN(0,1)");
}
bool DurablePendingModel::markRetry(std::uint64_t id, const std::string& node,
                                    std::uint32_t expectedCount,
                                    std::uint64_t nextAt, std::uint64_t nowMs) {
    auto connection = borrow();
    if (!connection || id == 0 || node.empty() || expectedCount >= 2) return false;
    auto* conn = connection->getConnection();
    if (!execute(conn, "UPDATE pending_delivery SET retry_count=retry_count+1,"
        "next_retry_at_ms=" + std::to_string(nextAt) +
        ",updated_at_ms=" + std::to_string(nowMs) +
        " WHERE delivery_id=" + std::to_string(id) + " AND owner_node=" +
        quoted(conn, node) + " AND state=0"
        " AND retry_count=" + std::to_string(expectedCount) +
        " AND next_retry_at_ms<=" + std::to_string(nowMs) +
        " AND expire_at_ms>" + std::to_string(nowMs))) return false;
    return mysql_affected_rows(conn) == 1;
}

std::optional<std::vector<DurablePendingRecord>> DurablePendingModel::claimForUser(
    int user, const std::string& node, std::uint64_t nowMs, std::size_t limit) {
    auto connection = borrow();
    if (!connection || user <= 0 || node.empty() || node.size() > 64) return std::nullopt;
    auto* conn = connection->getConnection();
    auto rows = queryRows(conn, "SELECT " + std::string(columns) +
        " FROM pending_delivery WHERE receiver_user_id=" + std::to_string(user) +
        " AND state IN(0,1) AND expire_at_ms>" + std::to_string(nowMs) +
        " ORDER BY delivery_id LIMIT " + std::to_string(std::min(limit, size_t{10000})));
    if (!rows) return std::nullopt;
    std::vector<DurablePendingRecord> claimed;
    for (auto& row : *rows) {
        // Recovery is scheduled by the regular durable retry scanner. Its
        // persisted CAS advances retry_count before any replayed send.
        const auto next = nowMs;
        if (!execute(conn, "UPDATE pending_delivery SET owner_node=" + quoted(conn, node) +
            ",state=0,next_retry_at_ms=" + std::to_string(next) +
            ",updated_at_ms=" + std::to_string(nowMs) +
            " WHERE delivery_id=" + std::to_string(row.deliveryId) +
            " AND state IN(0,1) AND expire_at_ms>" + std::to_string(nowMs)))
            return std::nullopt;
        if (mysql_affected_rows(conn) != 1) continue;
        row.ownerNode = node;
        row.state = DurableState::Pending;
        row.nextRetryAtMs = next;
        claimed.push_back(std::move(row));
    }
    return claimed;
}

std::optional<std::vector<DurablePendingRecord>> DurablePendingModel::dueForNode(
    const std::string& node, std::uint64_t nowMs, std::size_t limit) {
    auto connection = borrow();
    if (!connection || node.empty() || node.size() > 64) return std::nullopt;
    auto* conn = connection->getConnection();
    return queryRows(conn, "SELECT " + std::string(columns) +
        " FROM pending_delivery WHERE owner_node=" + quoted(conn, node) +
        " AND state=0 AND next_retry_at_ms<=" + std::to_string(nowMs) +
        " ORDER BY next_retry_at_ms,delivery_id LIMIT " +
        std::to_string(std::min(limit, size_t{100})));
}

std::optional<std::size_t> DurablePendingModel::recoverStartup(
    const std::string& node, std::uint64_t nowMs) {
    auto connection = borrow();
    if (!connection || node.empty() || node.size() > 64) return std::nullopt;
    auto* conn = connection->getConnection();
    if (!execute(conn, "UPDATE pending_delivery SET state=3,updated_at_ms=" +
        std::to_string(nowMs) + " WHERE owner_node=" + quoted(conn, node) +
        " AND state IN(0,1) AND expire_at_ms<=" + std::to_string(nowMs)))
        return std::nullopt;
    if (!execute(conn, "SELECT COUNT(*) FROM pending_delivery WHERE owner_node=" +
        quoted(conn, node) + " AND state IN(0,1)")) return std::nullopt;
    std::unique_ptr<MYSQL_RES, decltype(&mysql_free_result)> result(
        mysql_store_result(conn), &mysql_free_result);
    if (!result) return std::nullopt;
    auto row = mysql_fetch_row(result.get());
    if (!row || !row[0]) return std::nullopt;
    try { return std::stoull(row[0]); }
    catch (...) { return std::nullopt; }
}

std::optional<std::size_t> DurablePendingModel::expireOverdue(
    std::uint64_t nowMs, std::size_t limit) {
    auto connection = borrow();
    if (!connection) return std::nullopt;
    auto* conn = connection->getConnection();
    if (!execute(conn, "UPDATE pending_delivery SET state=3,updated_at_ms=" +
        std::to_string(nowMs) + " WHERE state IN(0,1) AND expire_at_ms<=" +
        std::to_string(nowMs) + " LIMIT " +
        std::to_string(std::min(limit, size_t{100})))) return std::nullopt;
    return mysql_affected_rows(conn);
}

bool DurablePendingModel::cleanupTerminal(std::uint64_t olderThanMs,
                                           std::size_t limit) {
    auto connection = borrow();
    if (!connection) return false;
    return execute(connection->getConnection(),
        "DELETE FROM pending_delivery WHERE state IN(2,3) AND updated_at_ms<" +
        std::to_string(olderThanMs) + " LIMIT " +
        std::to_string(std::min(limit, size_t{100})));
}
} // namespace gateway
