#!/usr/bin/env python3
"""Create/upgrade the ChatServer schema without putting admin secrets on argv."""

import argparse
import json
import os
from pathlib import Path
import re
import subprocess


ROOT = Path(__file__).resolve().parents[1]


def sql_literal(value):
    return "'" + str(value).replace("'", "''") + "'"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--config", default=str(ROOT / "conf/chatserver.json"))
    parser.add_argument("--admin-user", default=os.getenv("CHAT_DB_ADMIN_USER", "root"))
    args = parser.parse_args()
    db = json.loads(Path(args.config).read_text())["mysql"]
    if not re.fullmatch(r"[A-Za-z0-9_]+", db["dbname"]):
        raise SystemExit("mysql.dbname must contain only letters, digits, or underscore")

    env = os.environ.copy()
    admin_password = os.getenv("CHAT_DB_ADMIN_PASSWORD")
    if admin_password is not None:
        env["MYSQL_PWD"] = admin_password
    command = ["mysql", "--batch", "--skip-column-names", "--host", db["host"],
               "--port", str(db["port"]), "--user", args.admin_user]

    accounts = []
    for host in ("%", "localhost"):
        account = f"{sql_literal(db['user'])}@{sql_literal(host)}"
        accounts.extend((
            f"CREATE USER IF NOT EXISTS {account} IDENTIFIED BY {sql_literal(db['password'])}",
            f"ALTER USER {account} IDENTIFIED BY {sql_literal(db['password'])}",
            f"GRANT ALL PRIVILEGES ON `{db['dbname']}`.* TO {account}",
        ))
    bootstrap = ";".join((
        f"CREATE DATABASE IF NOT EXISTS `{db['dbname']}` CHARACTER SET utf8mb4",
        *accounts, "FLUSH PRIVILEGES",
    )) + ";"
    subprocess.run(command, input=bootstrap, text=True, env=env, check=True)
    database_command = command + ["--database", db["dbname"]]
    for name in ("chat.sql", "chat_history.sql", "pending_delivery.sql"):
        subprocess.run(database_command, input=(ROOT / "sql" / name).read_text(),
                       text=True, env=env, check=True)

    def query(statement):
        return subprocess.run(database_command + ["--execute", statement],
                              text=True, env=env, check=True,
                              capture_output=True).stdout.strip()

    nullable = query("SELECT IS_NULLABLE FROM information_schema.COLUMNS "
                     "WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME='pending_delivery' "
                     "AND COLUMN_NAME='owner_node'")
    if nullable == "NO":
        query("ALTER TABLE pending_delivery MODIFY owner_node VARCHAR(64) NULL")
    additions = (
        ("COLUMN", "legacy_chatmessage_id",
         "ALTER TABLE pending_delivery ADD COLUMN legacy_chatmessage_id BIGINT NULL"),
        ("INDEX", "uq_pending_legacy_chatmessage",
         "ALTER TABLE pending_delivery ADD UNIQUE KEY uq_pending_legacy_chatmessage (legacy_chatmessage_id)"),
        ("INDEX", "idx_pending_waiting_user",
         "ALTER TABLE pending_delivery ADD KEY idx_pending_waiting_user (state,receiver_user_id)"),
    )
    for kind, name, statement in additions:
        table = "COLUMNS" if kind == "COLUMN" else "STATISTICS"
        field = "COLUMN_NAME" if kind == "COLUMN" else "INDEX_NAME"
        exists = query(f"SELECT COUNT(*) FROM information_schema.{table} "
                       "WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME='pending_delivery' "
                       f"AND {field}={sql_literal(name)}")
        if exists == "0":
            query(statement)
    history_index = query("SELECT COUNT(*) FROM information_schema.STATISTICS "
                          "WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME='chatmessage' "
                          "AND INDEX_NAME='idx_chat_legacy_inbox'")
    if history_index == "0":
        query("ALTER TABLE chatmessage ADD KEY idx_chat_legacy_inbox (toid,status,id)")
    tables = query("SELECT COUNT(*) FROM information_schema.TABLES "
                   "WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME IN "
                   "('user','friend','allgroup','groupuser','chatmessage','pending_delivery')")
    if tables != "6":
        raise SystemExit(f"schema verification failed: expected 6 tables, got {tables}")
    if query("SELECT IS_NULLABLE FROM information_schema.COLUMNS "
             "WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME='pending_delivery' "
             "AND COLUMN_NAME='owner_node'") != "YES":
        raise SystemExit("schema verification failed: owner_node must be nullable")
    print(f"database {db['dbname']} is ready; schema is idempotent")


if __name__ == "__main__":
    main()
