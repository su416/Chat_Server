#!/usr/bin/env python3
"""
Protobuf 压测脚本 — 和 bench_onechat.py 对应，但走 Protobuf 协议
用法：python3 script/bench_proto.py
"""

import json
import socket
import struct
import threading
import time
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "proto_gen"))

# 尝试导入 protobuf 生成文件
try:
    import chat_pb2
except ImportError:
    print("请先生成 Python protobuf 绑定：")
    print("  protoc --python_out=script/proto_gen --proto_path=proto proto/chat.proto")
    sys.exit(1)

from bench_common import (
    HOST, PORT, CONNECT_TIMEOUT, RECV_TIMEOUT,
    LOGIN_MSG, REG_MSG, ONE_CHAT_MSG, GROUP_CHAT_MSG,
    compute_stats, print_result,
)

RUN_ID = f"benchpb_{int(time.time())}"
OUTPUT_DIR = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                          "docs", "bench_results")
CONCURRENCY_LEVELS = [10, 50, 100, 200, 500]
MESSAGES_PER_PAIR = 50
MESSAGES_PER_SENDER = 100
WARMUP_RATIO = 0.1


def ensure_output_dir():
    os.makedirs(OUTPUT_DIR, exist_ok=True)


# ==================== Protobuf 协议编解码 ====================

def send_proto_frame(sock, envelope):
    """发送 Protobuf 帧：4 字节 BE 长度前缀 + Envelope 序列化"""
    data = envelope.SerializeToString()
    sock.sendall(struct.pack('!I', len(data)) + data)


def recv_proto_frame(sock, timeout=RECV_TIMEOUT):
    """接收一帧 Protobuf 数据"""
    sock.settimeout(timeout)
    header = b""
    while len(header) < 4:
        chunk = sock.recv(4 - len(header))
        if not chunk:
            return None
        header += chunk
    length = struct.unpack('!I', header)[0]
    body = b""
    while len(body) < length:
        chunk = sock.recv(length - len(body))
        if not chunk:
            return None
        body += chunk
    env = chat_pb2.Envelope()
    env.ParseFromString(body)
    return env


def negotiate_protobuf(sock):
    """连接后第一条消息：协商切换到 Protobuf"""
    # 协商请求走 JSON（因为还没协商好）
    data = json.dumps({"msgid": 0, "accept": "protobuf"}).encode() + b'\0'
    sock.sendall(data)
    # 读回协商响应（JSON 格式，\0 分隔）
    sock.settimeout(RECV_TIMEOUT)
    buf = b""
    while b'\0' not in buf:
        chunk = sock.recv(4096)
        if not chunk: raise RuntimeError("negotiate failed: no response")
        buf += chunk
    resp = json.loads(buf.split(b'\0')[0])
    if resp.get("status") != "ok":
        raise RuntimeError(f"negotiate failed: {resp}")


def proto_login(sock, user_id, password):
    """Protobuf 登录"""
    req = chat_pb2.LoginRequest()
    req.id = user_id
    req.password = password

    env = chat_pb2.Envelope()
    env.msgid = LOGIN_MSG
    env.payload = req.SerializeToString()

    send_proto_frame(sock, env)
    return recv_proto_frame(sock)


def proto_register(run_id, index):
    """Protobuf 注册，返回 (user_id, username, password)"""
    username = f"{run_id}_u{index}"
    password = "123456"

    req = chat_pb2.RegisterRequest()
    req.name = username
    req.password = password

    env = chat_pb2.Envelope()
    env.msgid = REG_MSG
    env.payload = req.SerializeToString()

    with socket.create_connection((HOST, PORT), timeout=CONNECT_TIMEOUT) as sock:
        send_proto_frame(sock, env)
        resp = recv_proto_frame(sock)
        if resp is None:
            raise RuntimeError(f"register failed for {username}: no response")

        # 响应目前走桥接 —— body 是 JSON，errno=0 在响应 envelope 的 errcode 里
        if resp.errcode != 0:
            raise RuntimeError(f"register failed for {username}: errcode={resp.errcode}")

        # 解析 payload(JSON) 拿到新分配的 id
        try:
            js = json.loads(resp.payload)
            return js.get("id", -1), username, password
        except:
            # 若 payload 不是 JSON，尝试从 errcode 判断
            return -1, username, password


# ==================== 场景 A：在线到在线 ====================

def _pb_receiver(pair_idx, user_id, password, expected_msgs,
                 recv_stamps, errors, ready_event):
    """Protobuf 接收者线程"""
    sock = None
    try:
        sock = socket.create_connection((HOST, PORT), timeout=CONNECT_TIMEOUT)
        proto_login(sock, user_id, password)
        ready_event.set()

        received = 0
        while received < expected_msgs:
            resp = recv_proto_frame(sock, timeout=RECV_TIMEOUT)
            if resp is None:
                break
            if resp.msgid != ONE_CHAT_MSG:
                continue
            recv_stamps.append(time.perf_counter())
            received += 1
    except Exception as e:
        errors.append(f"[pb recv p{pair_idx}] {e}")
    finally:
        if sock:
            try: sock.close()
            except: pass


def run_online_to_online_pb(concurrency, messages_per_pair):
    print(f"\n{'='*60}")
    print(f"[PB online-to-online] concurrency={concurrency}, msgs/pair={messages_per_pair}")
    print(f"{'='*60}")

    pairs = []
    for i in range(concurrency):
        sid, _, spwd = proto_register(f"{RUN_ID}_c{concurrency}_oto", f"s{i}")
        rid, _, rpwd = proto_register(f"{RUN_ID}_c{concurrency}_oto", f"r{i}")
        if sid < 0 or rid < 0:
            raise RuntimeError(f"registration failed at pair {i}")
        pairs.append(((sid, spwd), (rid, rpwd)))
    print(f"  registered {len(pairs)} pairs")

    send_stamps = [[] for _ in range(concurrency)]
    recv_stamps = [[] for _ in range(concurrency)]
    recv_errors = []
    ready_events = [threading.Event() for _ in range(concurrency)]

    recv_threads = []
    for i, ((_, _), (rid, rpwd)) in enumerate(pairs):
        t = threading.Thread(target=_pb_receiver, args=(
            i, rid, rpwd, messages_per_pair, recv_stamps[i], recv_errors, ready_events[i]), daemon=True)
        t.start()
        recv_threads.append(t)

    for i, ev in enumerate(ready_events):
        if not ev.wait(timeout=30):
            print(f"  WARNING: receiver {i} not ready")
    print(f"  all receivers ready")

    send_start = time.perf_counter()
    for i, ((sid, spwd), (rid, _)) in enumerate(pairs):
        sock = None
        try:
            sock = socket.create_connection((HOST, PORT), timeout=CONNECT_TIMEOUT)
            proto_login(sock, sid, spwd)
            for j in range(messages_per_pair):
                send_stamps[i].append(time.perf_counter())
                req = chat_pb2.OneChatRequest()
                req.id = sid
                req.toid = rid
                req.name = f"pb_s{i}"
                req.msg = f"{RUN_ID}_pb_m{j}"
                req.time = time.strftime("%Y-%m-%d %H:%M:%S", time.localtime())
                env = chat_pb2.Envelope()
                env.msgid = ONE_CHAT_MSG
                env.payload = req.SerializeToString()
                send_proto_frame(sock, env)
        finally:
            if sock: sock.close()

    send_end = time.perf_counter()

    for t in recv_threads:
        t.join(timeout=max(5, messages_per_pair * 0.1))

    warmup_skip = max(1, int(messages_per_pair * WARMUP_RATIO))
    all_latencies = []
    total_recv = 0
    for pair_send, pair_recv in zip(send_stamps, recv_stamps):
        total_recv += len(pair_recv)
        n = min(len(pair_send), len(pair_recv))
        for k in range(warmup_skip, n):
            all_latencies.append((pair_recv[k] - pair_send[k]) * 1000.0)

    total_messages = concurrency * messages_per_pair
    elapsed = send_end - send_start
    qps = total_recv / elapsed if elapsed > 0 else 0.0

    result = {
        "concurrency": concurrency,
        "scenario": "online_to_online_pb",
        "total_messages": total_messages,
        "success_messages": total_recv,
        "failed_messages": total_messages - total_recv,
        "qps": round(qps, 1),
        "elapsed_seconds": round(elapsed, 3),
        "latency": compute_stats(all_latencies),
        "recv_errors": len(recv_errors),
    }
    print_result("PB online-to-online result", result)
    if recv_errors:
        for e in recv_errors[:3]:
            print(f"  [error] {e}")
    return result


# ==================== 主流程 ====================

def main():
    ensure_output_dir()
    print(f"RUN_ID = {RUN_ID}")
    print(f"Concurrency levels: {CONCURRENCY_LEVELS}")
    print(f"Protocol: Protobuf (4-byte length prefix + Envelope)")

    all_results = {
        "run_id": RUN_ID,
        "timestamp": time.strftime("%Y-%m-%d %H:%M:%S", time.localtime()),
        "server": f"{HOST}:{PORT}",
        "protocol": "protobuf",
        "online_to_online": [],
    }

    for concurrency in CONCURRENCY_LEVELS:
        try:
            r = run_online_to_online_pb(concurrency, MESSAGES_PER_PAIR)
            all_results["online_to_online"].append(r)
        except Exception as e:
            print(f"\n  [FATAL] concurrency={concurrency}: {e}")
            all_results["online_to_online"].append({
                "concurrency": concurrency, "scenario": "online_to_online_pb", "error": str(e),
            })

    output_file = os.path.join(OUTPUT_DIR, f"{RUN_ID}.json")
    with open(output_file, "w", encoding="utf-8") as f:
        json.dump(all_results, f, indent=2, ensure_ascii=False)

    print(f"\n{'='*60}")
    print(f"Report saved to: {output_file}")
    print(f"\nSummary:")
    for r in all_results["online_to_online"]:
        if "error" not in r:
            lat = r.get("latency", {})
            print(f"  c={r['concurrency']:4d} | qps={r['qps']:8.1f} | "
                  f"p50={lat.get('p50_ms', 0):6.1f}ms p99={lat.get('p99_ms', 0):6.1f}ms | "
                  f"success={r['success_messages']}/{r['total_messages']}")


if __name__ == "__main__":
    main()
