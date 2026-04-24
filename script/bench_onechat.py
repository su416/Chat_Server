#!/usr/bin/env python3
import json
import socket
import threading
import time
from typing import List, Tuple

HOST = "127.0.0.1"
PORT = 6000

# 改成你的离线接收者 id
RECEIVER_ID = 12

# 压测规模
CONCURRENCY = 10
MESSAGES_PER_USER = 20

# 超时配置
CONNECT_TIMEOUT = 5.0
LOGIN_RECV_TIMEOUT = 8.0

# 协议约定
LOGIN_MSG = 1
REG_MSG = 4
ONE_CHAT_MSG = 6

RUN_ID = f"bench_{int(time.time())}"


def send_json(sock: socket.socket, obj: dict) -> None:
    data = json.dumps(obj, ensure_ascii=False).encode("utf-8") + b"\0"
    sock.sendall(data)


def recv_one_json(sock: socket.socket, timeout: float = 5.0) -> dict:
    sock.settimeout(timeout)
    data = b""

    while True:
        chunk = sock.recv(4096)
        if not chunk:
            break

        data += chunk

        # 兼容服务端带 '\0' 的情况
        trimmed = data.rstrip(b"\0")

        try:
            return json.loads(trimmed.decode("utf-8"))
        except json.JSONDecodeError:
            # 可能还没收完整，继续收
            continue

    if not data:
        return {}

    return json.loads(data.rstrip(b"\0").decode("utf-8"))


def register_user(index: int) -> Tuple[int, str]:
    username = f"{RUN_ID}_user_{index}"
    password = "123456"

    with socket.create_connection((HOST, PORT), timeout=CONNECT_TIMEOUT) as sock:
        send_json(sock, {
            "msgid": REG_MSG,
            "name": username,
            "password": password,
        })
        resp = recv_one_json(sock, timeout=LOGIN_RECV_TIMEOUT)

    if resp.get("errno") != 0:
        raise RuntimeError(f"register failed for {username}: {resp}")

    return resp["id"], password


def worker(user_id: int, password: str, index: int, results: List[bool]) -> None:
    phase = "init"
    try:
        phase = "connect"
        with socket.create_connection((HOST, PORT), timeout=CONNECT_TIMEOUT) as sock:
            phase = "login"
            send_json(sock, {
                "msgid": LOGIN_MSG,
                "id": user_id,
                "password": password,
            })
            resp = recv_one_json(sock, timeout=LOGIN_RECV_TIMEOUT)
            if resp.get("errno") != 0:
                raise RuntimeError(f"login failed for {user_id}: {resp}")

            phase = "send"
            for j in range(MESSAGES_PER_USER):
                msg = f"{RUN_ID}_u{index}_m{j}"
                now = time.strftime("%Y-%m-%d %H:%M:%S", time.localtime())
                send_json(sock, {
                    "msgid": ONE_CHAT_MSG,
                    "id": user_id,
                    "toid": RECEIVER_ID,
                    "name": f"{RUN_ID}_user_{index}",
                    "msg": msg,
                    "time": now,
                })

        results[index] = True
    except Exception as e:
        print(f"[worker {index}] phase={phase} error: {e}")
        results[index] = False


def main() -> None:
    print(f"RUN_ID = {RUN_ID}")
    print("registering users...")

    users: List[Tuple[int, str]] = []
    for i in range(CONCURRENCY):
        uid, pwd = register_user(i)
        users.append((uid, pwd))

    print("registered users:")
    for i, (uid, _) in enumerate(users):
        print(f"  worker {i}: user_id={uid}")

    results = [False] * CONCURRENCY
    threads: List[threading.Thread] = []

    start = time.time()

    for i, (uid, pwd) in enumerate(users):
        t = threading.Thread(target=worker, args=(uid, pwd, i, results))
        t.start()
        threads.append(t)

    for t in threads:
        t.join()

    end = time.time()

    success_workers = sum(1 for x in results if x)
    requested_messages = CONCURRENCY * MESSAGES_PER_USER
    success_messages = success_workers * MESSAGES_PER_USER
    failed_workers = CONCURRENCY - success_workers
    elapsed = end - start
    avg = elapsed / success_messages if success_messages else 0.0

    print("\n===== RESULT =====")
    print(f"run_id={RUN_ID}")
    print(f"concurrency={CONCURRENCY}")
    print(f"messages_per_user={MESSAGES_PER_USER}")
    print(f"requested_messages={requested_messages}")
    print(f"success_workers={success_workers}/{CONCURRENCY}")
    print(f"failed_workers={failed_workers}")
    print(f"success_messages={success_messages}")
    print(f"elapsed_seconds={elapsed:.4f}")
    print(f"avg_seconds_per_success_message={avg:.6f}")
    print("==================")

    print("\nUse this SQL to verify:")
    print(f"select count(*) from chatmessage where msg like '{RUN_ID}%';")
    print("and:")
    print(f"select count(*) from offlinemessage where message like '%{RUN_ID}%';")


if __name__ == "__main__":
    main()
