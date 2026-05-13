#!/usr/bin/env python3
"""
单聊压测脚本 V2
- 场景 A：在线到在线（配对的发送者与接收者同时在线，测量端到端延迟）
- 场景 B：在线到离线（发送者发送给离线接收者，测量线下存储路径吞吐）
- 多并发梯度自动执行，生成 JSON 基线报告
"""

import json
import socket
import threading
import time
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from bench_common import (
    HOST, PORT, CONNECT_TIMEOUT, RECV_TIMEOUT,
    LOGIN_MSG, REG_MSG, ONE_CHAT_MSG,
    send_json, recv_one_json, register_user, login,
    compute_stats, print_result,
)

RUN_ID = f"bench1c_{int(time.time())}"
OUTPUT_DIR = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "docs", "bench_results")

# ========== 可调参数 ==========
# 多个并发梯度依次执行
CONCURRENCY_LEVELS = [10, 50, 100, 200, 500]
MESSAGES_PER_PAIR = 50       # 在线场景每对交换消息数
MESSAGES_PER_SENDER = 100    # 离线场景每发送者消息数
WARMUP_RATIO = 0.1           # 前 10% 消息不计入统计


def ensure_output_dir():
    os.makedirs(OUTPUT_DIR, exist_ok=True)


# ==================== 场景 A：在线到在线 ====================

def _receiver_thread(pair_idx: int, user_id: int, password: str,
                     expected_msgs: int, recv_stamps: list, errors: list,
                     ready_event: threading.Event):
    """接收者线程：保持长连接，收消息并记录接收时间戳"""
    sock = None
    try:
        sock = socket.create_connection((HOST, PORT), timeout=CONNECT_TIMEOUT)
        login(sock, user_id, password)
        ready_event.set()
        sock.settimeout(RECV_TIMEOUT)

        received = 0
        while received < expected_msgs:
            msg = recv_one_json(sock, timeout=RECV_TIMEOUT)
            if msg is None:
                break
            if msg.get("msgid") != ONE_CHAT_MSG:
                continue
            recv_stamps.append(time.perf_counter())
            received += 1
    except Exception as e:
        errors.append(f"[receiver p{pair_idx}] {e}")
    finally:
        if sock:
            try:
                sock.close()
            except Exception:
                pass


def _sender_thread(pair_idx: int, user_id: int, password: str,
                   receiver_id: int, messages: int, errors: list,
                   start_barrier: threading.Barrier,
                   send_stamps: list):
    """发送者线程：连接、登录、发送 M 条消息、断开"""
    sock = None
    try:
        sock = socket.create_connection((HOST, PORT), timeout=CONNECT_TIMEOUT)
        login(sock, user_id, password)
        # 等待所有发送者就绪
        start_barrier.wait(timeout=30)

        for j in range(messages):
            send_stamps.append(time.perf_counter())
            msg = {
                "msgid": ONE_CHAT_MSG,
                "id": user_id,
                "toid": receiver_id,
                "name": f"bench_s{pair_idx}",
                "msg": f"{RUN_ID}_oto_p{pair_idx}_m{j}",
                "time": time.strftime("%Y-%m-%d %H:%M:%S", time.localtime()),
            }
            send_json(sock, msg)
    except Exception as e:
        errors.append(f"[sender p{pair_idx}] {e}")
    finally:
        if sock:
            try:
                sock.close()
            except Exception:
                pass


def run_online_to_online(concurrency: int, messages_per_pair: int) -> dict:
    """
    concurrency 个发送-接收对，同时在线。
    发送方发完所有消息后断开，接收方计算每条消息的延迟。
    """
    print(f"\n{'='*60}")
    print(f"[online-to-online] concurrency={concurrency}, msgs/pair={messages_per_pair}")
    print(f"{'='*60}")

    # 1. 注册用户
    pairs = []  # [(sid, spwd), (rid, rpwd)]
    for i in range(concurrency):
        sid, _, spwd = register_user(f"{RUN_ID}_c{concurrency}_oto", f"s{i}")
        rid, _, rpwd = register_user(f"{RUN_ID}_c{concurrency}_oto", f"r{i}")
        pairs.append(((sid, spwd), (rid, rpwd)))
    print(f"  registered {len(pairs)} pairs")

    # 2. 启动接收者
    send_stamps = [[] for _ in range(concurrency)]   # 每条消息的发送时间戳
    recv_stamps = [[] for _ in range(concurrency)]   # 每条消息的接收时间戳
    receiver_errors = []
    ready_events = [threading.Event() for _ in range(concurrency)]

    recv_threads = []
    for i, ((_, _), (rid, rpwd)) in enumerate(pairs):
        t = threading.Thread(
            target=_receiver_thread,
            args=(i, rid, rpwd, messages_per_pair, recv_stamps[i], receiver_errors, ready_events[i]),
            daemon=True,
        )
        t.start()
        recv_threads.append(t)

    # 3. 等待所有接收者就绪
    for i, ev in enumerate(ready_events):
        if not ev.wait(timeout=30):
            print(f"  WARNING: receiver {i} did not become ready")
    print(f"  all receivers ready")

    # 4. 启动发送者（同步起跑）
    start_barrier = threading.Barrier(concurrency, timeout=30)
    sender_errors = []

    send_threads = []
    for i, ((sid, spwd), (rid, _)) in enumerate(pairs):
        t = threading.Thread(
            target=_sender_thread,
            args=(i, sid, spwd, rid, messages_per_pair, sender_errors, start_barrier, send_stamps[i]),
            daemon=True,
        )
        t.start()
        send_threads.append(t)

    # 5. 等待发送者完成
    send_start = time.perf_counter()
    for t in send_threads:
        t.join(timeout=60)
    send_end = time.perf_counter()

    # 6. 等待接收者收完（在发送者结束后给额外宽限期）
    grace_timeout = max(5, messages_per_pair * 0.1)
    for t in recv_threads:
        t.join(timeout=grace_timeout)

    # 7. 按序号匹配 send/recv 时间戳，计算端到端延迟
    warmup_skip = max(1, int(messages_per_pair * WARMUP_RATIO))
    all_latencies = []
    total_recv = 0
    for pair_send, pair_recv in zip(send_stamps, recv_stamps):
        total_recv += len(pair_recv)
        n = min(len(pair_send), len(pair_recv))
        for i in range(warmup_skip, n):
            all_latencies.append((pair_recv[i] - pair_send[i]) * 1000.0)

    total_messages = concurrency * messages_per_pair
    success_messages = total_recv
    failed_messages = total_messages - success_messages
    elapsed = send_end - send_start
    qps = success_messages / elapsed if elapsed > 0 else 0.0

    result = {
        "concurrency": concurrency,
        "scenario": "online_to_online",
        "total_messages": total_messages,
        "success_messages": success_messages,
        "failed_messages": failed_messages,
        "qps": round(qps, 1),
        "elapsed_seconds": round(elapsed, 3),
        "latency": compute_stats(all_latencies),
        "receiver_errors": len(receiver_errors),
        "sender_errors": len(sender_errors),
    }
    print_result("online-to-online result", result)

    if receiver_errors or sender_errors:
        for e in (receiver_errors + sender_errors)[:5]:
            print(f"  [error] {e}")

    return result


# ==================== 场景 B：在线到离线 ====================

def _offline_sender_thread(user_id: int, password: str, receiver_id: int,
                           messages: int, errors: list,
                           start_barrier: threading.Barrier,
                           send_times: list):
    """发送者线程：所有消息发往离线接收者"""
    sock = None
    try:
        sock = socket.create_connection((HOST, PORT), timeout=CONNECT_TIMEOUT)
        login(sock, user_id, password)
        start_barrier.wait(timeout=30)

        for j in range(messages):
            t0 = time.perf_counter()
            msg = {
                "msgid": ONE_CHAT_MSG,
                "id": user_id,
                "toid": receiver_id,
                "name": f"bench_off{user_id}",
                "msg": f"{RUN_ID}_otf_m{j}",
                "time": time.strftime("%Y-%m-%d %H:%M:%S", time.localtime()),
            }
            send_json(sock, msg)
            send_times.append(time.perf_counter() - t0)
    except Exception as e:
        errors.append(f"[offline sender] {e}")
    finally:
        if sock:
            try:
                sock.close()
            except Exception:
                pass


def run_online_to_offline(concurrency: int, messages_per_sender: int) -> dict:
    """
    concurrency 个发送者向 1 个离线接收者发消息。
    测量线下存储路径的吞吐。
    """
    print(f"\n{'='*60}")
    print(f"[online-to-offline] concurrency={concurrency}, msgs/sender={messages_per_sender}")
    print(f"{'='*60}")

    # 1. 注册 1 个接收者（永远不会登录）+ concurrency 个发送者
    rid, _, rpwd = register_user(f"{RUN_ID}_c{concurrency}_otf", "receiver")
    senders = []
    for i in range(concurrency):
        sid, _, spwd = register_user(f"{RUN_ID}_c{concurrency}_otf", f"s{i}")
        senders.append((sid, spwd))
    print(f"  registered receiver={rid}, {len(senders)} senders")

    # 2. 启动所有发送者（同步起跑）
    start_barrier = threading.Barrier(concurrency, timeout=30)
    all_send_times = []  # 每条消息的 send_json 耗时
    errors = []

    threads = []
    for sid, spwd in senders:
        t = threading.Thread(
            target=_offline_sender_thread,
            args=(sid, spwd, rid, messages_per_sender, errors, start_barrier, all_send_times),
            daemon=True,
        )
        t.start()
        threads.append(t)

    start = time.perf_counter()
    for t in threads:
        t.join(timeout=60)
    end = time.perf_counter()

    warmup_skip = max(1, int(len(all_send_times) * WARMUP_RATIO))
    measured = all_send_times[warmup_skip:]

    total_messages = concurrency * messages_per_sender
    elapsed = end - start
    qps = total_messages / elapsed if elapsed > 0 else 0.0

    # send_times 是秒，转换为毫秒
    latencies_ms = [t * 1000.0 for t in measured]

    result = {
        "concurrency": concurrency,
        "scenario": "online_to_offline",
        "total_messages": total_messages,
        "success_messages": len(measured),
        "failed_messages": total_messages - len(measured),
        "qps": round(qps, 1),
        "elapsed_seconds": round(elapsed, 3),
        "latency": compute_stats(latencies_ms),
        "errors": len(errors),
    }
    print_result("online-to-offline result", result)

    if errors:
        for e in errors[:5]:
            print(f"  [error] {e}")

    return result


# ==================== 主流程 ====================

def main():
    ensure_output_dir()
    print(f"RUN_ID = {RUN_ID}")
    print(f"Concurrency levels: {CONCURRENCY_LEVELS}")

    all_results = {
        "run_id": RUN_ID,
        "timestamp": time.strftime("%Y-%m-%d %H:%M:%S", time.localtime()),
        "server": f"{HOST}:{PORT}",
        "online_to_online": [],
        "online_to_offline": [],
    }

    for concurrency in CONCURRENCY_LEVELS:
        # 场景 A：在线到在线
        try:
            r = run_online_to_online(concurrency, MESSAGES_PER_PAIR)
            all_results["online_to_online"].append(r)
        except Exception as e:
            print(f"\n  [FATAL] online-to-online concurrency={concurrency}: {e}")
            all_results["online_to_online"].append({
                "concurrency": concurrency, "scenario": "online_to_online",
                "error": str(e),
            })

        # 场景 B：在线到离线
        try:
            r = run_online_to_offline(concurrency, MESSAGES_PER_SENDER)
            all_results["online_to_offline"].append(r)
        except Exception as e:
            print(f"\n  [FATAL] online-to-offline concurrency={concurrency}: {e}")
            all_results["online_to_offline"].append({
                "concurrency": concurrency, "scenario": "online_to_offline",
                "error": str(e),
            })

    # 输出汇总报告
    output_file = os.path.join(OUTPUT_DIR, f"{RUN_ID}.json")
    with open(output_file, "w", encoding="utf-8") as f:
        json.dump(all_results, f, indent=2, ensure_ascii=False)

    print(f"\n{'='*60}")
    print(f"Report saved to: {output_file}")
    print(f"Run ID: {RUN_ID}")
    print(f"\nSummary:")
    for scenario in ["online_to_online", "online_to_offline"]:
        for r in all_results[scenario]:
            if "error" not in r:
                lat = r.get("latency", {})
                print(f"  {r['scenario']:20s} | c={r['concurrency']:4d} | "
                      f"qps={r['qps']:8.1f} | "
                      f"p50={lat.get('p50_ms', 0):6.1f}ms p99={lat.get('p99_ms', 0):6.1f}ms | "
                      f"success={r['success_messages']}/{r['total_messages']}")


if __name__ == "__main__":
    main()
