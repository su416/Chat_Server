#!/usr/bin/env python3
"""Apply the additive Phase 6 schema using the existing server config.

The password is passed through MYSQL_PWD, never command-line arguments or logs.
"""
import json
import os
from pathlib import Path
import subprocess


ROOT = Path(__file__).resolve().parents[1]
config = json.loads((ROOT / "conf/chatserver.json").read_text())
db = config["mysql"]
env = os.environ.copy()
env["MYSQL_PWD"] = db["password"]
command = [
    "mysql", "--batch", "--skip-column-names",
    "--host", db["host"], "--port", str(db["port"]),
    "--user", db["user"], "--database", db["dbname"],
]
schema = (ROOT / "sql/pending_delivery.sql").read_text()
subprocess.run(command, input=schema, text=True, env=env, check=True)
check = subprocess.run(
    command + ["--execute", "SHOW TABLES LIKE 'pending_delivery'"],
    text=True, env=env, check=True, capture_output=True,
)
if check.stdout.strip() != "pending_delivery":
    raise SystemExit("Phase 6 migration verification failed")
print("Phase 6 pending_delivery table is ready")
