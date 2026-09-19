#!/bin/bash
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
RUN_DIR="$ROOT/run"
mkdir -p "$RUN_DIR"

if [ ! -x "$ROOT/bin/ChatServer" ]; then
  echo "ChatServer not found; run ./script/build.sh first" >&2
  exit 1
fi

for node in a b; do
  pid_file="$RUN_DIR/node-$node.pid"
  if [ -f "$pid_file" ] && kill -0 "$(cat "$pid_file")" 2>/dev/null; then
    echo "node-$node is already running (pid $(cat "$pid_file"))" >&2
    exit 1
  fi
done

cd "$ROOT"
nohup ./bin/ChatServer conf/chatserver-node-a.json >run/node-a.log 2>&1 &
echo $! >run/node-a.pid
nohup ./bin/ChatServer conf/chatserver-node-b.json >run/node-b.log 2>&1 &
echo $! >run/node-b.pid

sleep 1
for node in a b; do
  pid=$(cat "$RUN_DIR/node-$node.pid")
  if ! kill -0 "$pid" 2>/dev/null; then
    echo "node-$node failed; inspect $RUN_DIR/node-$node.log" >&2
    "$ROOT/script/stop_cluster.sh" || true
    exit 1
  fi
done
echo "cluster started: node-a=127.0.0.1:6000 node-b=127.0.0.1:6001"
