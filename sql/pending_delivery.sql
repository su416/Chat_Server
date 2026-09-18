-- Phase 6 additive migration. Run once against the existing chat database.
-- Existing chatmessage/history and offline status semantics are unchanged.
CREATE TABLE IF NOT EXISTS pending_delivery (
    delivery_id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
    route_id VARCHAR(192) NULL,
    owner_node VARCHAR(64) NOT NULL,
    receiver_user_id INT NOT NULL,
    message_type INT NOT NULL,
    payload MEDIUMTEXT NOT NULL,
    retry_count TINYINT UNSIGNED NOT NULL DEFAULT 0,
    next_retry_at_ms BIGINT UNSIGNED NOT NULL,
    expire_at_ms BIGINT UNSIGNED NOT NULL,
    state TINYINT UNSIGNED NOT NULL DEFAULT 0
        COMMENT '0=PENDING, 1=WAITING_SESSION, 2=ACKED, 3=EXPIRED',
    created_at_ms BIGINT UNSIGNED NOT NULL,
    updated_at_ms BIGINT UNSIGNED NOT NULL,
    PRIMARY KEY (delivery_id),
    UNIQUE KEY uq_pending_route_id (route_id),
    KEY idx_pending_owner_due (owner_node, state, next_retry_at_ms),
    KEY idx_pending_receiver (receiver_user_id, state, delivery_id),
    KEY idx_pending_expiry (state, expire_at_ms)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
