#!/usr/bin/env python3
"""
群聊压测脚本 V2
- 逐群大小档位执行：5 / 20 / 50 人
- 每档测试两个场景：
  A) 全部离线：发送者一人在线，其余离线，测量扇出到离线存储的吞吐
  B) 混合在线：部分成员在线接收，测量端到端扇出延迟
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
    LOGIN_MSG, REG_MSG, CREATE_GROUP_MSG, ADD_GROUP_MSG, GROUP_CHAT_MSG,
    send_json, recv_one_json, register_user, login,
    compute_stats, print_result,
)

RUN_ID = f"benchgc_{int(time.time())}"
OUTPUT_DIR = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "docs", "bench_results")

# ========== 可调参数 ==========
GROUP_SIZES = [5, 20, 50]          # 逐群大小测试
MESSAGES_PER_GROUP = 200           # 每个群发送的消息数
ONLINE_RATIO = 0.2                 # 混合在线场景在线比例 (20%)
WARMUP_RATIO = 0.1


def ensure_output_dir():
    os.makedirs(OUTPUT_DIR, exist_ok=True)


def create_and_populate_group(owner_id: int, owner_pwd: str, member_ids: list,
                              member_pwds: list, group_name: str) -> int:
    """创建群并让所有成员加入，返回 group_id。所有成员加入后断开（离线状态）。"""
    # owner 登录并建群
    with socket.create_connection((HOST, PORT), timeout=CONNECT_TIMEOUT) as sock:
        login(sock, owner_id, owner_pwd)
        send_json(sock, {
            "msgid": CREATE_GROUP_MSG,
            "id": owner_id,
            "groupname": group_name,
            "groupdesc": f"benchmark group {group_name}",
        })
        resp = recv_one_json(sock)
        if resp is None or resp.get("errno") != 0:
            raise RuntimeError(f"create group failed: {resp}")

    group_id = resp["groupid"]

    # 其他成员逐个登录、加群、断开
    for uid, pwd in zip(member_ids, member_pwds):
        with socket.create_connection((HOST, PORT), timeout=CONNECT_TIMEOUT) as sock:
            login(sock, uid, pwd)
            send_json(sock, {"msgid": ADD_GROUP_MSG, "id": uid, "groupid": group_id})
            resp = recv_one_json(sock)
            if resp is None or resp.get("errno") != 0:
                raise RuntimeError(f"join group failed for {uid}: {resp}")

    return group_id


# ==================== 场景 A：全部离线扇出 ====================

def run_all_offline(group_size: int, messages: int) -> dict:
    """
    1 个发送者在线的群，其余 group_size-1 人离线。
    所有消息扇出到线下存储。
    """
    print(f"\n{'='*60}")
    print(f"[group all-offline] group_size={group_size}, messages={messages}")
    print(f"{'='*60}")

    # 注册成员
    owner_id, owner_name, owner_pwd = register_user(f"{RUN_ID}_ao", "owner")
    member_ids = []
    member_pwds = []
    for i in range(1, group_size):
        mid, _, mpwd = register_user(f"{RUN_ID}_ao", f"m{i}")
        member_ids.append(mid)
        member_pwds.append(mpwd)
    print(f"  registered {group_size} users")

    # 创建群 + 所有人加入
    group_id = create_and_populate_group(
        owner_id, owner_pwd, member_ids, member_pwds,
        f"{RUN_ID}_ao_g",
    )
    print(f"  group_id={group_id}, all members joined (all now offline)")

    time.sleep(0.5)

    # owner 登录发消息
    send_times = []

    with socket.create_connection((HOST, PORT), timeout=CONNECT_TIMEOUT) as sock:
        login(sock, owner_id, owner_pwd)

        send_start = time.perf_counter()
        for j in range(messages):
            t0 = time.perf_counter()
            msg = {
                "msgid": GROUP_CHAT_MSG,
                "id": owner_id,
                "groupid": group_id,
                "name": owner_name,
                "msg": f"{RUN_ID}_ao_m{j}",
                "bench_ts": t0,
                "time": time.strftime("%Y-%m-%d %H:%M:%S", time.localtime()),
            }
            send_json(sock, msg)
            send_times.append(time.perf_counter() - t0)
        send_end = time.perf_counter()

    warmup_skip = max(1, int(messages * WARMUP_RATIO))
    measured = send_times[warmup_skip:]
    elapsed = send_end - send_start
    qps = messages / elapsed if elapsed > 0 else 0.0
    latencies_ms = [t * 1000.0 for t in measured]

    expected_offline = messages * (group_size - 1)
    expected_history = messages

    result = {
        "group_size": group_size,
        "scenario": "group_all_offline",
        "offline_recipients": group_size - 1,
        "total_messages": messages,
        "expected_offline_rows": expected_offline,
        "expected_history_rows": expected_history,
        "qps": round(qps, 1),
        "elapsed_seconds": round(elapsed, 3),
        "latency": compute_stats(latencies_ms),
        "group_id": group_id,
    }
    print_result("group all-offline result", result)
    print(f"  offline_fan_out: {group_size - 1} members -> {expected_offline} total offline inserts expected")
    return result


# ==================== 场景 B：混合在线扇出 ====================

def _group_receiver(user_id: int, password: str, expected_msgs: int,
                    latencies: list, errors: list, ready_event: threading.Event):
    """群消息接收者线程"""
    sock = None
    try:
        sock = socket.create_connection((HOST, PORT), timeout=CONNECT_TIMEOUT)
        login(sock, user_id, password)
        ready_event.set()

        received = 0
        while received < expected_msgs:
            msg = recv_one_json(sock, timeout=RECV_TIMEOUT)
            if msg is None:
                break
            if msg.get("msgid") != GROUP_CHAT_MSG:
                continue
            send_ts = msg.get("bench_ts", 0.0)
            if send_ts > 0:
                latencies.append((time.perf_counter() - send_ts) * 1000.0)
            received += 1
    except Exception as e:
        errors.append(f"[grp recv u{user_id}] {e}")
    finally:
        if sock:
            try:
                sock.close()
            except Exception:
                pass


def run_mixed_online(group_size: int, messages: int) -> dict:
    """
    group_size 人群，约 online_ratio 比例在线（含发送者）。
    在线成员通过 TCP 接收，离线成员走线下存储。
    """
    online_count = max(2, int(group_size * ONLINE_RATIO))  # 至少 2 个在线（1 发 + 1 收）
    print(f"\n{'='*60}")
    print(f"[group mixed-online] group_size={group_size}, online={online_count}, messages={messages}")
    print(f"{'='*60}")

    # 注册 owner + member_ids[0..online_count-2] 在线，其余离线
    owner_id, owner_name, owner_pwd = register_user(f"{RUN_ID}_mo", "owner")
    all_member_ids = []
    all_member_pwds = []
    for i in range(1, group_size):
        mid, _, mpwd = register_user(f"{RUN_ID}_mo", f"m{i}")
        all_member_ids.append(mid)
        all_member_pwds.append(mpwd)

    # 创建群 + 所有成员加入
    group_id = create_and_populate_group(
        owner_id, owner_pwd, all_member_ids, all_member_pwds,
        f"{RUN_ID}_mo_g",
    )
    print(f"  group_id={group_id}, all members joined (now offline)")

    time.sleep(0.5)

    # 挑选在线成员（前 online_count-1 个，加上 owner 就是 online_count 个在线）
    online_members = all_member_ids[:online_count - 1]
    online_pwds = all_member_pwds[:online_count - 1]

    # 启动在线接收者
    recv_latencies = [[] for _ in range(len(online_members))]
    recv_errors = []
    ready_events = [threading.Event() for _ in range(len(online_members))]

    recv_threads = []
    for i, (mid, mpwd) in enumerate(zip(online_members, online_pwds)):
        t = threading.Thread(
            target=_group_receiver,
            args=(mid, mpwd, messages, recv_latencies[i], recv_errors, ready_events[i]),
            daemon=True,
        )
        t.start()
        recv_threads.append(t)

    for i, ev in enumerate(ready_events):
        if not ev.wait(timeout=30):
            print(f"  WARNING: receiver {i} not ready")

    # owner 登录发消息
    sock = None
    try:
        sock = socket.create_connection((HOST, PORT), timeout=CONNECT_TIMEOUT)
        login(sock, owner_id, owner_pwd)

        send_start = time.perf_counter()
        for j in range(messages):
            msg = {
                "msgid": GROUP_CHAT_MSG,
                "id": owner_id,
                "groupid": group_id,
                "name": owner_name,
                "msg": f"{RUN_ID}_mo_m{j}",
                "bench_ts": time.perf_counter(),
                "time": time.strftime("%Y-%m-%d %H:%M:%S", time.localtime()),
            }
            send_json(sock, msg)
        send_end = time.perf_counter()
    finally:
        if sock:
            sock.close()

    # 等待接收者收完
    for t in recv_threads:
        t.join(timeout=15)

    # 汇总延迟
    warmup_skip = max(1, int(messages * WARMUP_RATIO))
    all_latencies = []
    for bucket in recv_latencies:
        if len(bucket) > warmup_skip:
            all_latencies.extend(bucket[warmup_skip:])

    elapsed = send_end - send_start
    total_received = len(all_latencies)
    expected_online_received = len(online_members) * messages
    # 离线部分
    offline_count = group_size - 1 - len(online_members)
    expected_offline_rows = offline_count * messages if offline_count > 0 else 0

    result = {
        "group_size": group_size,
        "scenario": "group_mixed_online",
        "online_members": online_count,
        "offline_recipients": offline_count,
        "total_messages": messages,
        "expected_online_received": expected_online_received,
        "actual_online_received": total_received,
        "expected_offline_rows": expected_offline_rows,
        "qps": round(messages / elapsed, 1) if elapsed > 0 else 0.0,
        "elapsed_seconds": round(elapsed, 3),
        "latency": compute_stats(all_latencies),
        "group_id": group_id,
    }
    print_result("group mixed-online result", result)
    if recv_errors:
        for e in recv_errors[:3]:
            print(f"  [error] {e}")
    return result


# ==================== 主流程 ====================

def main():
    ensure_output_dir()
    print(f"RUN_ID = {RUN_ID}")
    print(f"Group sizes: {GROUP_SIZES}")

    all_results = {
        "run_id": RUN_ID,
        "timestamp": time.strftime("%Y-%m-%d %H:%M:%S", time.localtime()),
        "server": f"{HOST}:{PORT}",
        "group_all_offline": [],
        "group_mixed_online": [],
    }

    for group_size in GROUP_SIZES:
        # 场景 A：全部离线扇出
        try:
            r = run_all_offline(group_size, MESSAGES_PER_GROUP)
            all_results["group_all_offline"].append(r)
        except Exception as e:
            print(f"\n  [FATAL] all-offline group_size={group_size}: {e}")
            all_results["group_all_offline"].append({
                "group_size": group_size, "scenario": "group_all_offline", "error": str(e),
            })

        # 场景 B：混合在线
        try:
            r = run_mixed_online(group_size, MESSAGES_PER_GROUP)
            all_results["group_mixed_online"].append(r)
        except Exception as e:
            print(f"\n  [FATAL] mixed-online group_size={group_size}: {e}")
            all_results["group_mixed_online"].append({
                "group_size": group_size, "scenario": "group_mixed_online", "error": str(e),
            })

    # 输出报告
    output_file = os.path.join(OUTPUT_DIR, f"{RUN_ID}.json")
    with open(output_file, "w", encoding="utf-8") as f:
        json.dump(all_results, f, indent=2, ensure_ascii=False)

    print(f"\n{'='*60}")
    print(f"Report saved to: {output_file}")
    print(f"Run ID: {RUN_ID}")
    print(f"\nSummary:")
    for scenario in ["group_all_offline", "group_mixed_online"]:
        for r in all_results[scenario]:
            if "error" not in r:
                lat = r.get("latency", {})
                print(f"  {r['scenario']:20s} | size={r['group_size']:3d} | "
                      f"qps={r['qps']:8.1f} | "
                      f"p50={lat.get('p50_ms', 0):6.1f}ms p99={lat.get('p99_ms', 0):6.1f}ms")


if __name__ == "__main__":
    main()
