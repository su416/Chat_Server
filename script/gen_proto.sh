#!/bin/bash
set -e

PROJECT_ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUTPUT_DIR="$PROJECT_ROOT/build/generated/proto"

mkdir -p "$OUTPUT_DIR"

protoc \
  -I="$PROJECT_ROOT/proto" \
  --cpp_out="$OUTPUT_DIR" \
  "$PROJECT_ROOT/proto/chat.proto"

echo "protobuf generated in: $OUTPUT_DIR"
ls -l "$OUTPUT_DIR"