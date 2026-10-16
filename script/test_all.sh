#!/bin/bash
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
FULL=false
[ "${1:-}" = "--full" ] && FULL=true

cd "$ROOT"
./script/build.sh
ctest --test-dir build --output-on-failure
./script/gen_python_proto.sh
python3 script/traffic_governance_integration.py
python3 script/phase6_phase3_regression.py
python3 script/phase4_two_node_integration.py
python3 script/phase5_stream_integration.py
python3 script/phase6_durable_pending_integration.py
python3 script/phase7_offline_inbox_integration.py --backlog 100 --extended

if $FULL; then
  python3 script/phase7_offline_inbox_integration.py --backlog 500
  python3 script/phase7_offline_inbox_integration.py --backlog 1000
  python3 script/phase8_backpressure_integration.py --case db
  python3 script/phase8_backpressure_integration.py --case slow
  python3 script/phase8_backpressure_integration.py --case redis
  python3 script/phase8_local_benchmark.py --pairs 10 --messages 100 --protocol json --linger 8 --receive-timeout 25
  python3 script/phase8_local_benchmark.py --pairs 20 --messages 100 --protocol json --linger 8 --receive-timeout 30
  python3 script/phase8_local_benchmark.py --pairs 50 --messages 100 --protocol json --linger 8 --receive-timeout 35
fi
