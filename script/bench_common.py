#!/usr/bin/env python3
"""共享工具：JSON 分帧、用户注册、延迟统计、结果输出"""

import json
import socket
import time
import statistics
from typing import List, Tuple, Optional


HOST = "127.0.0.1"
PORT = 6000
CONNECT_TIMEOUT = 5.0
RECV_TIMEOUT = 10.0

# 消息号
LOGIN_MSG = 1
REG_MSG = 4
ONE_CHAT_MSG = 6
CREATE_GROUP_MSG = 8
ADD_GROUP_MSG = 9
GROUP_CHAT_MSG = 10


def send_json(sock: socket.socket, obj: dict) -> None:
    data = json.dumps(obj, ensure_ascii=False).encode("utf-8") + b"\0"
    sock.sendall(data)


def recv_one_json(sock: socket.socket, timeout: float = RECV_TIMEOUT) -> Optional[dict]:
    """从 socket 读取一条 \0 分隔的 JSON 消息。
    使用内部缓冲区处理 TCP 粘包：一次 recv 可能包含多条消息，
    剩余数据留在缓冲区供下次调用使用。"""
    if not hasattr(sock, "_bench_buf"):
        sock._bench_buf = b""

    sock.settimeout(timeout)

    while True:
        # 先检查缓冲区中是否已有完整消息
        parts = sock._bench_buf.split(b"\0", 1)
        if len(parts) == 2:
            sock._bench_buf = parts[1]
            try:
                return json.loads(parts[0].decode("utf-8"))
            except json.JSONDecodeError:
                continue  # 损坏的数据，丢弃，继续找下一条

        # 需要读取更多数据
        try:
            chunk = sock.recv(4096)
        except socket.timeout:
            return None
        if not chunk:
            break
        sock._bench_buf += chunk

    if not sock._bench_buf:
        return None
    # 连接关闭，尝试解析剩余数据
    parts = sock._bench_buf.split(b"\0", 1)
    try:
        return json.loads(parts[0].decode("utf-8"))
    except json.JSONDecodeError:
        return None


def register_user(run_id: str, index: int) -> Tuple[int, str, str]:
    username = f"{run_id}_u{index}"
    password = "123456"
    with socket.create_connection((HOST, PORT), timeout=CONNECT_TIMEOUT) as sock:
        send_json(sock, {"msgid": REG_MSG, "name": username, "password": password})
        resp = recv_one_json(sock)
    if resp is None or resp.get("errno") != 0:
        raise RuntimeError(f"register failed for {username}: {resp}")
    return resp["id"], username, password


def login(sock: socket.socket, user_id: int, password: str) -> dict:
    send_json(sock, {"msgid": LOGIN_MSG, "id": user_id, "password": password})
    resp = recv_one_json(sock)
    if resp is None or resp.get("errno") != 0:
        raise RuntimeError(f"login failed for {user_id}: {resp}")
    return resp


def compute_stats(values: List[float]) -> dict:
    """计算延迟分布"""
    if not values:
        return {"count": 0, "avg_ms": 0, "p50_ms": 0, "p95_ms": 0, "p99_ms": 0, "p999_ms": 0, "min_ms": 0, "max_ms": 0}
    sorted_vals = sorted(values)
    n = len(sorted_vals)
    return {
        "count": n,
        "avg_ms": round(statistics.mean(sorted_vals), 2),
        "p50_ms": round(sorted_vals[int(n * 0.50)], 2) if n > 1 else round(sorted_vals[0], 2),
        "p95_ms": round(sorted_vals[int(n * 0.95)], 2) if n > 10 else round(sorted_vals[-1], 2),
        "p99_ms": round(sorted_vals[int(n * 0.99)], 2) if n > 50 else round(sorted_vals[-1], 2),
        "p999_ms": round(sorted_vals[int(n * 0.999)], 2) if n > 500 else round(sorted_vals[-1], 2),
        "min_ms": round(sorted_vals[0], 2),
        "max_ms": round(sorted_vals[-1], 2),
    }


def print_result(title: str, result: dict) -> None:
    """格式化输出一轮结果"""
    print(f"\n--- {title} ---")
    print(f"  concurrency    = {result.get('concurrency')}")
    print(f"  scenario       = {result.get('scenario')}")
    print(f"  total_messages = {result.get('total_messages')}")
    print(f"  success        = {result.get('success_messages')}")
    print(f"  failed         = {result.get('failed_messages')}")
    print(f"  qps            = {result.get('qps', 0):.1f}")
    print(f"  elapsed        = {result.get('elapsed_seconds', 0):.2f}s")
    lat = result.get("latency", {})
    print(f"  latency (ms)   = avg={lat.get('avg_ms')} | p50={lat.get('p50_ms')} | p95={lat.get('p95_ms')} | p99={lat.get('p99_ms')}")
