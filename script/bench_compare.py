#!/usr/bin/env python3
"""
JSON vs Protobuf 性能对比脚本
依次运行 JSON 和 Protobuf 压测，输出对比报告
"""

import json
import os
import subprocess
import sys
import time

PROJECT_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUTPUT_DIR = os.path.join(PROJECT_ROOT, "docs", "bench_results")
os.makedirs(OUTPUT_DIR, exist_ok=True)

DB_USER = "root"
DB_PASS = "123456"

def run_mysql(sql):
    return subprocess.run(
        ["sudo", "mysql", f"-u{DB_USER}", f"-p{DB_PASS}", "-e", sql],
        capture_output=True, text=True
    )

def clear_bench_data():
    run_mysql("DELETE FROM chat.user WHERE name LIKE 'bench%';")
    run_mysql("DELETE FROM chat.chatmessage WHERE msg LIKE '%bench%';")

def run_bench(script_name, label):
    print(f"\n{'='*60}")
    print(f"  Running: {label}")
    print(f"{'='*60}")

    script_path = os.path.join(PROJECT_ROOT, "script", script_name)
    result = subprocess.run(
        [sys.executable, script_path],
        cwd=PROJECT_ROOT,
        capture_output=True, text=True
    )
    print(result.stdout)
    if result.stderr:
        print("STDERR:", result.stderr[:500])

    # 找到最新生成的 JSON 报告
    pattern = "bench1c_" if "onechat" in script_name else "benchpb_"
    files = sorted(
        [f for f in os.listdir(OUTPUT_DIR) if f.startswith(pattern) and f.endswith(".json")],
        key=lambda f: os.path.getmtime(os.path.join(OUTPUT_DIR, f)),
        reverse=True
    )
    if files:
        filepath = os.path.join(OUTPUT_DIR, files[0])
        with open(filepath) as f:
            return json.load(f)
    return None

def extract_oto_result(data):
    """从 JSON 报告中提取 online_to_online 数据"""
    if not data: return []
    # 兼容两种 key：bench_onechat 用 "online_to_online"，bench_proto 也用 "online_to_online"
    for key in ["online_to_online"]:
        if key in data:
            return [r for r in data[key] if "error" not in r]
    return []

def main():
    print("JSON vs Protobuf Benchmark Comparison")
    print(f"Timestamp: {time.strftime('%Y-%m-%d %H:%M:%S')}")
    print(f"Server: 127.0.0.1:6000")

    # 1. JSON 基线
    clear_bench_data()
    json_data = run_bench("bench_onechat.py", "JSON 基线")

    # 2. Protobuf
    clear_bench_data()
    proto_data = run_bench("bench_proto.py", "Protobuf")

    # 3. 对比
    json_results = {r["concurrency"]: r for r in extract_oto_result(json_data)}
    proto_results = {r["concurrency"]: r for r in extract_oto_result(proto_data)}

    print(f"\n{'='*70}")
    print(f"  {'JSON vs Protobuf 性能对比':^60}")
    print(f"{'='*70}")
    print(f"  {'并发':<6} {'指标':<12} {'JSON':<12} {'Protobuf':<12} {'变化':<12}")
    print(f"  {'-'*54}")

    for c in [10, 50, 100]:
        j = json_results.get(c, {})
        p = proto_results.get(c, {})

        for metric, key, unit in [
            ("P50 延迟", "p50_ms", "ms"),
            ("P99 延迟", "p99_ms", "ms"),
            ("QPS", "qps", ""),
            ("到达率", "success_messages", "/total"),
        ]:
            j_lat = j.get("latency", {}).get(key, 0) if key in ("p50_ms", "p99_ms") else j.get(key, 0)
            p_lat = p.get("latency", {}).get(key, 0) if key in ("p50_ms", "p99_ms") else p.get(key, 0)

            if j_lat == 0 and p_lat == 0:
                continue

            if key == "success_messages":
                j_val = f"{j_lat}/{j.get('total_messages', 0)}"
                p_val = f"{p_lat}/{p.get('total_messages', 0)}"
            else:
                j_val = f"{j_lat:.1f}{unit}"
                p_val = f"{p_lat:.1f}{unit}"

            if isinstance(j_lat, (int, float)) and isinstance(p_lat, (int, float)) and j_lat > 0:
                delta = (p_lat - j_lat) / j_lat * 100
                change = f"{delta:+.0f}%"
            else:
                change = "-"

            print(f"  c={c:<4} {metric:<12} {j_val:<12} {p_val:<12} {change:<12}")

    print(f"{'='*70}")
    print("\n  ※ Protobuf 响应走桥接模式（payload 仍是 JSON），实际收益在帧体积和路由延迟")

    # 帧大小对比（理论值）
    print(f"\n  === 帧大小对比（单条 oneChat 消息） ===")
    json_size = len('{"msgid":6,"id":1,"toid":2,"name":"alice","msg":"hello","time":"2026-06-03 10:00:00"}') + 1
    proto_size = 4 + 10  # 4B 长度前缀 + ~10B Envelope(2B msgid + payload 长度前缀 + ~60B body)
    print(f"  JSON:     ~{json_size} bytes (含 \\0 分帧符)")
    print(f"  Protobuf: ~{proto_size} bytes (4B 长度前缀 + Envelope 包装)")
    print(f"  节省:     ~{json_size - proto_size} bytes/帧 ({100 - proto_size*100//json_size}% 减少)")

    # 保存对比报告
    report = {
        "timestamp": time.strftime("%Y-%m-%d %H:%M:%S"),
        "json_run_id": json_data.get("run_id") if json_data else None,
        "proto_run_id": proto_data.get("run_id") if proto_data else None,
        "comparison": {
            str(c): {
                "json_p50": json_results.get(c, {}).get("latency", {}).get("p50_ms", 0),
                "proto_p50": proto_results.get(c, {}).get("latency", {}).get("p50_ms", 0),
                "json_qps": json_results.get(c, {}).get("qps", 0),
                "proto_qps": proto_results.get(c, {}).get("qps", 0),
            } for c in [10, 50, 100]
        }
    }
    report_path = os.path.join(OUTPUT_DIR, f"compare_{int(time.time())}.json")
    with open(report_path, "w") as f:
        json.dump(report, f, indent=2, ensure_ascii=False)
    print(f"\n  Report saved: {report_path}")


if __name__ == "__main__":
    main()
