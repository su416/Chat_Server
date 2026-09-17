#!/usr/bin/env python3
"""Phase 3 真实连接的未知/迟到 ACK、断线和 Binary 兼容验证。"""

import argparse
import json
import socket
import struct
import time

from bench_common import register_user
from reliable_delivery_integration import Wire, connect_and_login, HOST, PORT


def users(label):
    run_id = f"phase3edge_{label}_{int(time.time())}"
    return register_user(run_id, "sender"), register_user(run_id, "receiver")


def send_chat(wire, sender, receiver, marker):
    wire.send({"msgid": 6, "id": sender[0], "toid": receiver[0],
               "name": sender[1], "msg": marker,
               "time": "2026-09-17 00:00:00"})


def ack(wire, delivery):
    wire.send({"msgid": 20, "delivery_id": delivery["delivery_id"]})


def ack_edges():
    sender, receiver = users("ack")
    sender_socket, sender_wire = connect_and_login(sender, "json")
    receiver_socket, receiver_wire = connect_and_login(receiver, "json")
    try:
        receiver_wire.send({"msgid": 20, "delivery_id": "99999999"})
        sender_wire.send({"msgid": 6, "id": sender[0], "toid": receiver[0],
                          "name": sender[1], "msg": "wrong-session",
                          "time": "2026-09-17 00:00:00", "delivery_id": "123"})
        first = receiver_wire.recv(5)
        assert first["msg"] == "wrong-session"
        assert first["delivery_id"] != "123", "sender forged delivery identity"
        ack(sender_wire, first)  # 发送端不能确认接收端的投递。
        repeated = receiver_wire.recv(3.5)
        assert repeated and repeated["delivery_id"] == first["delivery_id"]
        ack(receiver_wire, repeated)
        ack(receiver_wire, repeated)  # 重复 ACK 无害。
        print("WRONG_SESSION_ACK_REJECTED=PASS", flush=True)
        print("DUPLICATE_AND_UNKNOWN_ACK_SAFE=PASS", flush=True)

        send_chat(sender_wire, sender, receiver, "late-ack")
        second = receiver_wire.recv(5)
        assert second["msg"] == "late-ack"
        # 不 ACK，等待 2 次重试及最终失败，期间消费重试帧。
        for _ in range(2):
            repeat = receiver_wire.recv(3)
            assert repeat and repeat["delivery_id"] == second["delivery_id"]
        time.sleep(2.5)
        ack(receiver_wire, second)  # 已从 PendingTable 删除。
        send_chat(sender_wire, sender, receiver, "still-connected")
        third = receiver_wire.recv(5)
        assert third["msg"] == "still-connected"
        ack(receiver_wire, third)
        print("LATE_ACK_IGNORED_CONNECTION_SURVIVES=PASS", flush=True)
        time.sleep(1)
    finally:
        receiver_socket.close()
        sender_socket.close()


def disconnect():
    sender, receiver = users("disconnect")
    sender_socket, sender_wire = connect_and_login(sender, "json")
    receiver_socket, receiver_wire = connect_and_login(receiver, "json")
    try:
        send_chat(sender_wire, sender, receiver, "old-session")
        delivery = receiver_wire.recv(5)
        assert delivery and delivery.get("delivery_id")
        receiver_socket.close()  # 不 ACK。
        time.sleep(3)
        new_socket, new_wire = connect_and_login(receiver, "json")
        try:
            assert new_wire.recv(1) is None, "old pending leaked to new session"
            print("DISCONNECT_PENDING_CANCELLED_NO_RECONNECT_REPLAY=PASS", flush=True)
            time.sleep(2)
        finally:
            new_socket.close()
    finally:
        sender_socket.close()


def binary_compatibility():
    sender, receiver = users("binary")
    sender_socket, sender_wire = connect_and_login(sender, "json")
    receiver_socket = socket.create_connection((HOST, PORT), timeout=5)
    receiver_wire = Wire(receiver_socket, "json")
    try:
        login = json.dumps({"id": receiver[0], "password": receiver[2]}).encode()
        receiver_socket.sendall(struct.pack("!BBH", 0xFF, 1, len(login)) + login)
        response = receiver_wire.recv(10)
        assert response and response.get("errno") == 0, response
        send_chat(sender_wire, sender, receiver, "binary-legacy")
        delivery = receiver_wire.recv(5)
        assert delivery and delivery.get("msg") == "binary-legacy"
        assert "delivery_id" not in delivery, delivery
        assert receiver_wire.recv(2.7) is None, "legacy Binary received retry"
        print("BINARY_LEGACY_NO_RETRY=PASS", flush=True)
        time.sleep(1)
    finally:
        receiver_socket.close()
        sender_socket.close()


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("case", choices=("ack", "disconnect", "binary"))
    name = parser.parse_args().case
    {"ack": ack_edges, "disconnect": disconnect,
     "binary": binary_compatibility}[name]()
