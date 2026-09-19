#!/usr/bin/env python3
"""Repeatable local reliable-path load run; prints raw workload and log location."""

import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

import phase4_two_node_integration as harness


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--pairs", type=int, required=True)
    parser.add_argument("--messages", type=int, default=100)
    parser.add_argument("--protocol", choices=("json", "protobuf"), default="json")
    parser.add_argument("--linger", type=float, default=0)
    parser.add_argument("--receive-timeout", type=float, default=30)
    parser.add_argument("--db-queue", type=int)
    parser.add_argument("--db-workers", type=int)
    parser.add_argument("--db-delay-ms", type=int, default=0)
    arguments = parser.parse_args()
    if arguments.pairs < 1 or arguments.pairs > 100 or arguments.messages < 1:
        parser.error("pairs must be 1..100 and messages must be positive")
    folder = Path(tempfile.mkdtemp(prefix="phase8-local-bench-"))
    print(f"LOG_DIR={folder}", flush=True)
    flow = {}
    if arguments.db_queue:
        flow["db_max_queue_size"] = arguments.db_queue
    if arguments.db_workers:
        flow["db_worker_threads"] = arguments.db_workers
    env = {"CHAT_PHASE8_TEST_DB_WORKER_DELAY_MS": str(arguments.db_delay_ms)} if arguments.db_delay_ms else None
    process, log = harness.start_server(folder, "A", 6000,
                                        extra_env=env, flow=flow)
    try:
        command = [sys.executable, str(harness.ROOT / "script/reliable_delivery_integration.py"),
                   "--protocol", arguments.protocol, "--senders", str(arguments.pairs),
                   "--messages", str(arguments.messages), "--linger", str(arguments.linger),
                   "--receive-timeout", str(arguments.receive_timeout), "--allow-rejection"]
        run = subprocess.run(command, cwd=harness.ROOT, env=os.environ,
                             text=True, capture_output=True)
        if run.returncode:
            print(run.stdout, end="")
            print(run.stderr, file=sys.stderr, end="")
            raise SystemExit(run.returncode)
        result = json.loads(run.stdout)
        print(json.dumps({"log_dir": str(folder), "workload": result}, indent=2))
    finally:
        harness.stop_server(process)
        log.close()


if __name__ == "__main__":
    main()
