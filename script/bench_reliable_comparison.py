#!/usr/bin/env python3
"""Identical real-socket workload for Phase 5 vs Phase 6 comparison."""

import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

import phase4_two_node_integration as p


def main():
    folder = Path(tempfile.mkdtemp(prefix="phase-compare-"))
    server, log = p.start_server(folder, "A", 6000)
    try:
        command = [sys.executable, str(p.ROOT / "script/reliable_delivery_integration.py"),
                   "--protocol", "json", "--senders", "10", "--messages", "10",
                   "--linger", "0"]
        result = subprocess.run(command, cwd=p.ROOT, env=os.environ,
                                text=True, capture_output=True, check=True)
        parsed = json.loads(result.stdout)
        print(json.dumps({"log_dir": str(folder), "workload": "JSON 10x10",
                          "sent": parsed["sent"],
                          "delivered_unique": parsed["delivered_unique"],
                          "qps_unique": parsed["qps_unique"],
                          "elapsed_seconds": parsed["elapsed_seconds"],
                          "latency": parsed["latency"]}, indent=2), flush=True)
    finally:
        p.stop_server(server)
        log.close()


if __name__ == "__main__":
    main()
