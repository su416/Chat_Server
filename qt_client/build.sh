#!/bin/bash
set -e

PROJECT_ROOT=$(cd "$(dirname "$0")" && pwd)
QT_PATH="/home/ys/Qt/6.11.0/gcc_64"

rm -rf "$PROJECT_ROOT/build"
mkdir -p "$PROJECT_ROOT/build"
cd "$PROJECT_ROOT/build"

cmake .. -DCMAKE_PREFIX_PATH="$QT_PATH"
make -j$(nproc)

echo ""
echo "build done: $PROJECT_ROOT/build/bin/ChatClient"
