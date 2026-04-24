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

# 协议约定
LOGIN_MSG = 1
REG_MSG = 4
ONE_CHAT_MSG = 6

RUN_ID = f"bench_{int(time.time())}"


def send_json(sock: socket.socket, obj: dict) -> None:
    data = json.dumps(obj, ensure_ascii=False).encode("utf-8") + b"\0"
    sock.sendall(data)


def recv_one_json(sock: socket.socket, timeout: float = 3.0) -> dict:
    sock.settimeout(timeout)
    data = b""

    while True:
        chunk = sock.recv(4096)
        if not chunk:
            break

        data += chunk

        # 兼容未来如果服务端加了 '\0'
        trimmed = data.rstrip(b"\0")

        try:
            return json.loads(trimmed.decode("utf-8"))
        except json.JSONDecodeError:
            # 还没收完整，继续收
            continue

    if not data:
        return {}

    return json.loads(data.rstrip(b"\0").decode("utf-8"))

def register_user(index: int) -> Tuple[int, str]:
    username = f"{RUN_ID}_user_{index}"
    password = "123456"

    with socket.create_connection((HOST, PORT), timeout=3) as sock:
        send_json(sock, {
            "msgid": REG_MSG,
            "name": username,
            "password": password,
        })
        resp = recv_one_json(sock)

    if resp.get("errno") != 0:
        raise RuntimeError(f"register failed for {username}: {resp}")

    return resp["id"], password


def worker(user_id: int, password: str, index: int, results: List[bool]) -> None:
    try:
        with socket.create_connection((HOST, PORT), timeout=3) as sock:
            # 登录
            send_json(sock, {
                "msgid": LOGIN_MSG,
                "id": user_id,
                "password": password,
            })
            resp = recv_one_json(sock)
            if resp.get("errno") != 0:
                raise RuntimeError(f"login failed for {user_id}: {resp}")

            # 发消息
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
        print(f"[worker {index}] error: {e}")
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
    threads = []

    start = time.time()

    for i, (uid, pwd) in enumerate(users):
        t = threading.Thread(target=worker, args=(uid, pwd, i, results))
        t.start()
        threads.append(t)

    for t in threads:
        t.join()

    end = time.time()

    success_workers = sum(1 for x in results if x)
    total_messages = CONCURRENCY * MESSAGES_PER_USER
    elapsed = end - start
    avg = elapsed / total_messages if total_messages else 0.0

    print("\n===== RESULT =====")
    print(f"run_id={RUN_ID}")
    print(f"concurrency={CONCURRENCY}")
    print(f"messages_per_user={MESSAGES_PER_USER}")
    print(f"total_messages={total_messages}")
    print(f"success_workers={success_workers}/{CONCURRENCY}")
    print(f"elapsed_seconds={elapsed:.4f}")
    print(f"avg_seconds_per_message={avg:.6f}")
    print("==================")

    print("\nUse this SQL to verify:")
    print(f"select count(*) from chatmessage where msg like '{RUN_ID}%';")
    print("and:")
    print(f"select count(*) from offlinemessage where message like '%{RUN_ID}%';")


if __name__ == "__main__":
    main()
