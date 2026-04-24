#!/bin/bash
set -e

PROJECT_ROOT=$(cd "$(dirname "$0")/.." && pwd)

cd "$PROJECT_ROOT"

if [ ! -f "./bin/ChatClient" ]; then
    echo "ChatClient not found, please build project first."
    echo "Try: ./script/build.sh"
    exit 1
fi

SERVER_IP=${1:-127.0.0.1}
SERVER_PORT=${2:-6000}

echo "starting ChatClient, server=${SERVER_IP}:${SERVER_PORT}"
./bin/ChatClient "$SERVER_IP" "$SERVER_PORT"
