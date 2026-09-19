#!/bin/bash
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
RUN_DIR="$ROOT/run"

for node in a b; do
  pid_file="$RUN_DIR/node-$node.pid"
  [ -f "$pid_file" ] || continue
  pid=$(cat "$pid_file")
  if kill -0 "$pid" 2>/dev/null; then
    kill -INT "$pid"
    for _ in $(seq 1 100); do
      kill -0 "$pid" 2>/dev/null || break
      sleep 0.1
    done
    if kill -0 "$pid" 2>/dev/null; then
      echo "node-$node did not stop within 10 seconds" >&2
      exit 1
    fi
  fi
  rm -f "$pid_file"
done
echo "cluster stopped"
