#!/bin/bash
set -e

PROJECT_ROOT=$(cd "$(dirname "$0")/.." && pwd)

cd "$PROJECT_ROOT"

rm -rf build
mkdir -p build

# 先生成 protobuf 代码
./script/gen_proto.sh

cd build
cmake ..
make -j$(nproc)