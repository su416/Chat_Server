#!/bin/bash
set -euo pipefail

PROJECT_ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUTPUT_DIR="$PROJECT_ROOT/script/proto_gen"

mkdir -p "$OUTPUT_DIR"
protoc -I "$PROJECT_ROOT/proto" --python_out="$OUTPUT_DIR" \
  "$PROJECT_ROOT/proto/chat.proto"
echo "Python protobuf binding generated: $OUTPUT_DIR/chat_pb2.py"
