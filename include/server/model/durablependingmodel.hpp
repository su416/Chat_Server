#ifndef DURABLE_PENDING_MODEL_HPP
#define DURABLE_PENDING_MODEL_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace gateway {

enum class DurableState : int { Pending = 0, WaitingSession = 1, Acked = 2, Expired = 3 };

struct DurablePendingRecord {
    std::uint64_t deliveryId = 0;
    std::string routeId;
    std::string ownerNode;
    int receiverUserId = 0;
    int messageType = 0;
    std::string payload;
    std::uint32_t retryCount = 0;
    std::uint64_t nextRetryAtMs = 0;
    std::uint64_t expireAtMs = 0;
    DurableState state = DurableState::Pending;
};

enum class PersistStatus { Inserted, Existing, Error };
enum class DurableAckStatus { Completed, AlreadyCompleted, Terminal, Error };
struct PersistResult {
    PersistStatus status = PersistStatus::Error;
    DurablePendingRecord record;
};

// MySQL is the source of truth. All calls are blocking and must run on DB workers.
class DurablePendingModel final {
public:
    PersistResult persist(const std::string& routeId, const std::string& nodeId,
                          int receiverUserId, int messageType,
                          const std::string& payload, std::uint64_t nowMs);
    PersistResult persistOffline(int receiverUserId, int messageType,
                                 const std::string& payload, std::uint64_t nowMs,
                                 const std::string& routeId = "");
    // Idempotent legacy drain keyed by chatmessage.id; never trusts JSON equality.
    std::optional<std::size_t> migrateLegacyForUser(int receiverUserId,
                                                    std::uint64_t nowMs,
                                                    std::size_t limit = 100);
    DurableAckStatus markAcked(std::uint64_t deliveryId, int receiverUserId,
                               std::uint64_t nowMs);
    std::optional<bool> markWaiting(std::uint64_t deliveryId, const std::string& nodeId,
                                    std::uint64_t expectedNextRetryAtMs, std::uint64_t nowMs);
    bool markWaitingForUser(int receiverUserId, const std::string& nodeId,
                            std::uint64_t nowMs);
    bool transferOwner(std::uint64_t deliveryId,
                       const std::string& expectedOwner,
                       const std::string& newOwner, std::uint64_t nowMs);
    std::optional<bool> markExpired(std::uint64_t deliveryId, std::uint64_t nowMs);
    bool markRetry(std::uint64_t deliveryId, const std::string& nodeId,
                   std::uint32_t expectedCount,
                   std::uint64_t nextRetryAtMs, std::uint64_t nowMs);
    std::optional<std::vector<DurablePendingRecord>> claimForUser(
        int receiverUserId, const std::string& nodeId,
        std::uint64_t nowMs, std::size_t limit = 32,
        std::size_t maxBytes = 16 * 1024 * 1024);
    std::optional<std::pair<std::size_t, std::size_t>> activeCounts();
    std::optional<std::vector<int>> waitingUsersAfter(int userId,
                                                      std::size_t limit = 100);
    std::optional<std::vector<DurablePendingRecord>> dueForNode(
        const std::string& nodeId, std::uint64_t nowMs, std::size_t limit = 100);
    // Returns count of active records assigned to this node, nullopt on DB error.
    std::optional<std::size_t> recoverStartup(const std::string& nodeId,
                                              std::uint64_t nowMs);
    std::optional<std::size_t> expireOverdue(std::uint64_t nowMs,
                                             std::size_t limit = 100);
    bool cleanupTerminal(std::uint64_t olderThanMs, std::size_t limit = 100);
};

std::uint64_t unixMillis();

} // namespace gateway
#endif
