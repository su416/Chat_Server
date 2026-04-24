#!/bin/bash
set -e

PROJECT_ROOT=$(cd "$(dirname "$0")/.." && pwd)

cd "$PROJECT_ROOT"

if [ ! -f "./bin/ChatServer" ]; then
    echo "ChatServer not found, please build project first."
    echo "Try: ./script/build.sh"
    exit 1
fi

echo "starting ChatServer..."
./bin/ChatServer
