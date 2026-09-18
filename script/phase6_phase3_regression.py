#!/usr/bin/env python3
"""Run Phase 3 reliable-client regression against the Phase 6 server."""

import os
from pathlib import Path
import subprocess
import sys
import tempfile

import phase4_two_node_integration as p
from phase6_durable_pending_integration import sql


def main():
    folder = Path(tempfile.mkdtemp(prefix="phase6-phase3-"))
    print(f"LOG_DIR={folder}", flush=True)
    server, log = p.start_server(folder, "A", 6000)
    command = [sys.executable, "-u", str(p.ROOT / "script/reliable_delivery_integration.py")]
    scenarios = [
        ("json_1x5", ["--protocol", "json", "--senders", "1", "--messages", "5"]),
        ("protobuf_1x5", ["--protocol", "protobuf", "--typed-ack",
                          "--senders", "1", "--messages", "5"]),
        ("json_10x10", ["--protocol", "json", "--senders", "10",
                         "--messages", "10", "--linger", "0"]),
        ("ack_loss_1x5", ["--protocol", "json", "--senders", "1",
                           "--messages", "5", "--drop-ack", "first10"]),
    ]
    try:
        for name, args in scenarios:
            before = int(sql("SELECT COUNT(*) FROM pending_delivery"))
            result = subprocess.run(command + args, cwd=p.ROOT, env=os.environ,
                                    text=True, capture_output=True)
            print(f"SCENARIO={name} exit={result.returncode}", flush=True)
            print(result.stdout, flush=True)
            if result.returncode:
                print(result.stderr, flush=True)
                raise RuntimeError(f"Phase 3 regression failed: {name}")
            after = int(sql("SELECT COUNT(*) FROM pending_delivery"))
            print(f"DURABLE_ROWS_ADDED={after - before}", flush=True)
        print("PHASE3_RELIABLE_REGRESSION=PASS", flush=True)
    finally:
        p.stop_server(server)
        log.close()


if __name__ == "__main__":
    main()
