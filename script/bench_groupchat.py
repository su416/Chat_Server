#!/usr/bin/env python3
import json
import socket
import time
from typing import List, Tuple

HOST = "127.0.0.1"
PORT = 6000

# 压测规模
GROUP_SIZE = 5               # 群总人数（含发送者）
MESSAGES_PER_SENDER = 100    # 发送者发多少条群消息

# 超时配置
CONNECT_TIMEOUT = 5.0
RECV_TIMEOUT = 8.0

# 协议消息号（按你当前项目约定）
LOGIN_MSG = 1
REG_MSG = 4
CREATE_GROUP_MSG = 8
ADD_GROUP_MSG = 9
GROUP_CHAT_MSG = 10

RUN_ID = f"bench_group_{int(time.time())}"


def send_json(sock: socket.socket, obj: dict) -> None:
    data = json.dumps(obj, ensure_ascii=False).encode("utf-8") + b"\0"
    sock.sendall(data)


def recv_one_json(sock: socket.socket, timeout: float = RECV_TIMEOUT) -> dict:
    sock.settimeout(timeout)
    data = b""

    while True:
        chunk = sock.recv(4096)
        if not chunk:
            break

        data += chunk
        trimmed = data.rstrip(b"\0")

        try:
            return json.loads(trimmed.decode("utf-8"))
        except json.JSONDecodeError:
            continue

    if not data:
        return {}

    return json.loads(data.rstrip(b"\0").decode("utf-8"))


def register_user(index: int) -> Tuple[int, str, str]:
    username = f"{RUN_ID}_user_{index}"
    password = "123456"

    with socket.create_connection((HOST, PORT), timeout=CONNECT_TIMEOUT) as sock:
        send_json(sock, {
            "msgid": REG_MSG,
            "name": username,
            "password": password,
        })
        resp = recv_one_json(sock)

    if resp.get("errno") != 0:
        raise RuntimeError(f"register failed for {username}: {resp}")

    return resp["id"], username, password


def login(sock: socket.socket, user_id: int, password: str) -> dict:
    send_json(sock, {
        "msgid": LOGIN_MSG,
        "id": user_id,
        "password": password,
    })
    resp = recv_one_json(sock)
    if resp.get("errno") != 0:
        raise RuntimeError(f"login failed for {user_id}: {resp}")
    return resp


def create_group(sock: socket.socket, owner_id: int) -> Tuple[int, str]:
    group_name = f"{RUN_ID}_group"
    group_desc = "group benchmark"

    send_json(sock, {
        "msgid": CREATE_GROUP_MSG,
        "id": owner_id,
        "groupname": group_name,
        "groupdesc": group_desc,
    })
    resp = recv_one_json(sock)
    if resp.get("errno") != 0:
        raise RuntimeError(f"create group failed: {resp}")

    group_id = resp.get("groupid")
    if not group_id:
        raise RuntimeError(f"create group ack missing groupid: {resp}")

    return group_id, group_name


def join_group(sock: socket.socket, user_id: int, group_id: int) -> None:
    send_json(sock, {
        "msgid": ADD_GROUP_MSG,
        "id": user_id,
        "groupid": group_id,
    })
    resp = recv_one_json(sock)
    if resp.get("errno") != 0:
        raise RuntimeError(f"join group failed for user {user_id}: {resp}")


def main() -> None:
    print(f"RUN_ID = {RUN_ID}")
    print("registering users...")

    users: List[Tuple[int, str, str]] = []
    for i in range(GROUP_SIZE):
        uid, uname, pwd = register_user(i)
        users.append((uid, uname, pwd))

    print("registered users:")
    for i, (uid, uname, _) in enumerate(users):
        print(f"  member {i}: user_id={uid}, username={uname}")

    owner_id, owner_name, owner_pwd = users[0]

    # 1. owner 登录并建群
    with socket.create_connection((HOST, PORT), timeout=CONNECT_TIMEOUT) as owner_sock:
        login(owner_sock, owner_id, owner_pwd)
        group_id, group_name = create_group(owner_sock, owner_id)
        print(f"created group: group_id={group_id}, group_name={group_name}")

    # 2. 其他成员逐个登录并加群，然后断开，保持离线
    for uid, uname, pwd in users[1:]:
        with socket.create_connection((HOST, PORT), timeout=CONNECT_TIMEOUT) as sock:
            login(sock, uid, pwd)
            join_group(sock, uid, group_id)
            print(f"joined group: user_id={uid}, username={uname}")

    # 给服务端一点时间把断开连接的成员状态置为 offline
    time.sleep(0.5)

    # 3. owner 再次登录，单人在线发送群消息
    start = time.time()

    with socket.create_connection((HOST, PORT), timeout=CONNECT_TIMEOUT) as sender_sock:
        login(sender_sock, owner_id, owner_pwd)

        for i in range(MESSAGES_PER_SENDER):
            msg = f"{RUN_ID}_m{i}"
            now = time.strftime("%Y-%m-%d %H:%M:%S", time.localtime())
            send_json(sender_sock, {
                "msgid": GROUP_CHAT_MSG,
                "id": owner_id,
                "groupid": group_id,
                "name": owner_name,
                "msg": msg,
                "time": now,
            })

    end = time.time()

    elapsed = end - start
    success_messages = MESSAGES_PER_SENDER
    expected_history_rows = MESSAGES_PER_SENDER
    expected_offline_rows = MESSAGES_PER_SENDER * (GROUP_SIZE - 1)
    avg = elapsed / success_messages if success_messages else 0.0

    print("\n===== RESULT =====")
    print(f"run_id={RUN_ID}")
    print(f"group_id={group_id}")
    print(f"group_size={GROUP_SIZE}")
    print(f"messages_per_sender={MESSAGES_PER_SENDER}")
    print(f"success_messages={success_messages}")
    print(f"expected_history_rows={expected_history_rows}")
    print(f"expected_offline_rows={expected_offline_rows}")
    print(f"elapsed_seconds={elapsed:.4f}")
    print(f"avg_seconds_per_message={avg:.6f}")
    print("==================")

    print("\nUse these SQLs to verify:")
    print(f"select count(*) from chatmessage where msg like '{RUN_ID}%';")
    print(f"select count(*) from offlinemessage where message like '%{RUN_ID}%';")


if __name__ == "__main__":
    main()
