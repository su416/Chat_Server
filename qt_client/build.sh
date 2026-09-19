#!/bin/bash
set -e

PROJECT_ROOT=$(cd "$(dirname "$0")" && pwd)
QT_ROOT=${QT_ROOT:-}

rm -rf "$PROJECT_ROOT/build"
mkdir -p "$PROJECT_ROOT/build"
cd "$PROJECT_ROOT/build"

if [ -n "$QT_ROOT" ]; then
    cmake .. -DCMAKE_PREFIX_PATH="$QT_ROOT"
else
    cmake ..
fi
make -j$(nproc)

echo ""
echo "build done: $PROJECT_ROOT/build/bin/ChatClient"
