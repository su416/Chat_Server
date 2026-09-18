#!/usr/bin/env python3
"""Idempotent Phase 7 additive schema migration (no legacy data deletion)."""
import json
import os
from pathlib import Path
import subprocess


ROOT = Path(__file__).resolve().parents[1]
db = json.loads((ROOT / "conf/chatserver.json").read_text())["mysql"]
env = {**os.environ, "MYSQL_PWD": db["password"]}
command = ["mysql", "--batch", "--skip-column-names", "--host", db["host"],
           "--port", str(db["port"]), "--user", db["user"],
           "--database", db["dbname"]]


def query(statement):
    return subprocess.run(command + ["--execute", statement], env=env,
                          text=True, capture_output=True, check=True).stdout.strip()


subprocess.run(command, input=(ROOT / "sql/pending_delivery.sql").read_text(),
               env=env, text=True, check=True)
nullable = query("SELECT IS_NULLABLE FROM information_schema.COLUMNS "
                 "WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME='pending_delivery' "
                 "AND COLUMN_NAME='owner_node'")
if nullable == "NO":
    query("ALTER TABLE pending_delivery MODIFY owner_node VARCHAR(64) NULL")
elif nullable != "YES":
    raise SystemExit("pending_delivery.owner_node is missing")

legacy_column = query("SELECT COUNT(*) FROM information_schema.COLUMNS "
                      "WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME='pending_delivery' "
                      "AND COLUMN_NAME='legacy_chatmessage_id'")
if legacy_column == "0":
    query("ALTER TABLE pending_delivery ADD COLUMN "
          "legacy_chatmessage_id BIGINT NULL")
legacy_index = query("SELECT COUNT(*) FROM information_schema.STATISTICS "
                     "WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME='pending_delivery' "
                     "AND INDEX_NAME='uq_pending_legacy_chatmessage'")
if legacy_index == "0":
    query("ALTER TABLE pending_delivery ADD UNIQUE KEY "
          "uq_pending_legacy_chatmessage (legacy_chatmessage_id)")
waiting_index = query("SELECT COUNT(*) FROM information_schema.STATISTICS "
                      "WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME='pending_delivery' "
                      "AND INDEX_NAME='idx_pending_waiting_user'")
if waiting_index == "0":
    query("ALTER TABLE pending_delivery ADD KEY "
          "idx_pending_waiting_user (state,receiver_user_id)")
history_index = query("SELECT COUNT(*) FROM information_schema.STATISTICS "
                      "WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME='chatmessage' "
                      "AND INDEX_NAME='idx_chat_legacy_inbox'")
if history_index == "0":
    query("ALTER TABLE chatmessage ADD KEY idx_chat_legacy_inbox (toid,status,id)")
assert query("SELECT IS_NULLABLE FROM information_schema.COLUMNS "
             "WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME='pending_delivery' "
             "AND COLUMN_NAME='owner_node'") == "YES"
print("Phase 7 pending_delivery schema is ready")
