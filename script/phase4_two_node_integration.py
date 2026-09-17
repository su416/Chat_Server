#!/usr/bin/env python3
"""真实 Redis/MySQL + 两个 ChatServer 的 Phase 4 故障注入。运行前先构建并生成 chat_pb2。"""

import json
import os
import signal
import socket
import subprocess
import tempfile
import threading
import time
from pathlib import Path

from bench_common import register_user
from reliable_delivery_integration import Wire
from reliable_client import RecentDeliveryIds


ROOT = Path(__file__).resolve().parent.parent
RUN_ID = f"phase4_{int(time.time())}_{os.getpid()}"


def start_server(folder, node, port, redis_port=6379):
    config = json.loads((ROOT / "conf/chatserver.json").read_text())
    config["server"].update({"node_id": node, "name": f"ChatServer-{node}", "port": port})
    config["redis"]["port"] = redis_port
    path = folder / f"{node}.json"
    path.write_text(json.dumps(config))
    log = (folder / f"{node}.log").open("w+")
    process = subprocess.Popen(["stdbuf", "-oL", "-eL",
                                str(ROOT / "bin/ChatServer"), str(path)],
                               cwd=ROOT, stdout=log, stderr=subprocess.STDOUT)
    deadline = time.monotonic() + 12
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError(f"{node} exited during startup; log={log.name}")
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.2):
                return process, log
        except OSError:
            time.sleep(0.1)
    raise RuntimeError(f"{node} did not listen on {port}")


def stop_server(process):
    if process.poll() is not None:
        return
    process.send_signal(signal.SIGINT)
    try:
        process.wait(timeout=12)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=5)


def client(user, port, protocol):
    sock = socket.create_connection(("127.0.0.1", port), timeout=5)
    try:
        wire = Wire(sock, protocol)
        wire.send({"msgid": 1, "id": user[0], "password": user[2]})
        reply = wire.recv(10)
        if not reply or reply.get("errno") != 0:
            raise RuntimeError(f"login node port={port} protocol={protocol} user={user[0]}: {reply}")
        return sock, wire
    except Exception as error:
        sock.close()
        raise RuntimeError(f"client stage=negotiate/login node_port={port} "
                           f"protocol={protocol} utc={time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime())}: {error}") from error


def send_one(sender_wire, sender, receiver, marker):
    sender_wire.send({"msgid": 6, "id": sender[0], "toid": receiver[0],
                      "name": sender[1], "msg": marker,
                      "time": "2026-09-17 00:00:00"})


def receive_one(receiver_wire, marker):
    reply = receiver_wire.recv(6)
    assert reply and reply.get("msgid") == 6 and reply.get("msg") == marker, reply
    assert reply.get("delivery_id"), reply
    return reply


def ack(wire, reply):
    wire.send({"msgid": 20, "delivery_id": reply["delivery_id"]},
              typed_ack=(wire.protocol == "protobuf"))


def redis(*args):
    return subprocess.check_output(["redis-cli", "--raw", *map(str, args)],
                                   text=True).strip()


def test_pair(label, sender_protocol, receiver_protocol):
    sender = register_user(RUN_ID, f"{label}s")
    receiver = register_user(RUN_ID, f"{label}r")
    sender_sock, sender_wire = client(sender, 6000, sender_protocol)
    receiver_sock, receiver_wire = client(receiver, 6001, receiver_protocol)
    try:
        marker = f"{RUN_ID}-{label}"
        send_one(sender_wire, sender, receiver, marker)
        delivered = receive_one(receiver_wire, marker)
        ack(receiver_wire, delivered)
        print(f"PASS {sender_protocol}->{receiver_protocol} route_id!=delivery_id delivery={delivered['delivery_id']}", flush=True)
    finally:
        receiver_sock.close()
        sender_sock.close()


def test_ack_loss():
    sender = register_user(RUN_ID, "ack-s")
    receiver = register_user(RUN_ID, "ack-r")
    sender_sock, sender_wire = client(sender, 6000, "json")
    receiver_sock, receiver_wire = client(receiver, 6001, "protobuf")
    try:
        marker = f"{RUN_ID}-lost-ack"
        send_one(sender_wire, sender, receiver, marker)
        first = receive_one(receiver_wire, marker)
        recent = RecentDeliveryIds(4096)
        assert recent.observe(first["delivery_id"])
        repeated = receive_one(receiver_wire, marker)
        assert repeated["delivery_id"] == first["delivery_id"]
        assert not recent.observe(repeated["delivery_id"]), "client dedup failed"
        ack(receiver_wire, repeated)
        print("PASS cross-node ACK loss: same deliveryId retried; client dedup key unchanged", flush=True)
    finally:
        receiver_sock.close()
        sender_sock.close()


def test_reconnect_and_stale_route():
    sender = register_user(RUN_ID, "move-s")
    receiver = register_user(RUN_ID, "move-r")
    sender_sock, sender_wire = client(sender, 6000, "json")
    old_sock, old_wire = client(receiver, 6001, "json")
    new_sock, new_wire = client(receiver, 6000, "protobuf")
    try:
        assert redis("GET", f"chat:presence:user:{receiver[0]}").startswith("A|")
        marker = f"{RUN_ID}-move"
        send_one(sender_wire, sender, receiver, marker)
        ack(new_wire, receive_one(new_wire, marker))
        assert old_wire.recv(0.5) is None, "old owner received after reconnect"
        print("PASS B->A reconnect: new owner receives, old Session does not", flush=True)

        stale = {"version": 1, "route_id": f"{RUN_ID}-stale", "source_node": "A",
                 "target_node": "B", "message_type": 6,
                 "sender_user_id": sender[0], "receiver_user_id": receiver[0],
                 "hops": 0,
                 "payload": {"msgid": 6, "id": sender[0], "toid": receiver[0],
                             "msg": f"{RUN_ID}-stale", "name": sender[1]}}
        assert int(redis("PUBLISH", "chat:route:node:B", json.dumps(stale))) > 0
        ack(new_wire, receive_one(new_wire, stale["payload"]["msg"]))
        assert old_wire.recv(0.5) is None, "stale route delivered to old Session"
        print("PASS stale route: at most one reroute; no stale client delivery", flush=True)

        old_sock.close()  # Delayed old-node disconnect must not delete A.
        time.sleep(0.5)
        assert redis("GET", f"chat:presence:user:{receiver[0]}").startswith("A|")
        marker = f"{RUN_ID}-after-old-close"
        send_one(sender_wire, sender, receiver, marker)
        ack(new_wire, receive_one(new_wire, marker))
        print("PASS delayed old disconnect: A ownership preserved", flush=True)
    finally:
        new_sock.close()
        old_sock.close()
        sender_sock.close()


def test_simultaneous_login():
    user = register_user(RUN_ID, "race-owner")
    results = {}
    errors = []
    gate = threading.Barrier(2)

    def login_at(node, port):
        try:
            gate.wait(timeout=5)
            results[node] = client(user, port, "json")
        except Exception as error:
            errors.append(str(error))

    left = threading.Thread(target=login_at, args=("A", 6000))
    right = threading.Thread(target=login_at, args=("B", 6001))
    left.start(); right.start(); left.join(); right.join()
    assert not errors and len(results) == 2, errors
    try:
        owner = redis("GET", f"chat:presence:user:{user[0]}").split("|")[0]
        assert owner in ("A", "B"), owner
        loser = "B" if owner == "A" else "A"
        # Losing local Session is still physically connected but cannot send as owner.
        results[loser][1].send({"msgid": 6, "id": user[0], "toid": user[0],
                                 "name": user[1], "msg": "stale-sender"})
        reply = results[loser][1].recv(5)
        assert reply and reply.get("msgid") == 18 and reply.get("errno") == 6, reply
        print(f"PASS simultaneous login: one Redis owner={owner}; loser rejected", flush=True)
    finally:
        for sock, _ in results.values():
            sock.close()


def test_dead_node(process_b):
    receiver = register_user(RUN_ID, "dead-r")
    sender = register_user(RUN_ID, "dead-s")
    sock, _ = client(receiver, 6001, "json")
    sender_sock, sender_wire = client(sender, 6000, "json")
    try:
        assert redis("GET", f"chat:presence:user:{receiver[0]}").startswith("B|")
        time.sleep(5.5)  # Capture a complete B metric snapshot before SIGKILL.
        process_b.kill()  # Intentional kill -9, no graceful cleanup.
        process_b.wait(timeout=5)
        time.sleep(0.4)
        send_one(sender_wire, sender, receiver, f"{RUN_ID}-route-fail")
        failed = sender_wire.recv(5)
        assert failed and failed.get("errno") == 6, failed
        print("PASS Redis route failure: publish has no subscriber, explicit sender error", flush=True)
        time.sleep(22)
        assert redis("TTL", "chat:node:B") == "-2"
        # Lookup cleans stale presence atomically; no manual Redis edits.
        send_one(sender_wire, sender, receiver, f"{RUN_ID}-dead")
        time.sleep(0.5)
        assert redis("GET", f"chat:presence:user:{receiver[0]}") == ""
        new_sock, _ = client(receiver, 6000, "json")
        new_sock.close()
        print("PASS kill -9 B: lease expired, stale presence cleared, user relogged on A", flush=True)
    finally:
        sock.close()
        sender_sock.close()


def test_redis_unavailable(folder):
    user = register_user(RUN_ID, "redis-down")
    process, log = start_server(folder, "C", 6002, redis_port=6399)
    try:
        sock = socket.create_connection(("127.0.0.1", 6002), timeout=5)
        try:
            wire = Wire(sock, "json")
            wire.send({"msgid": 1, "id": user[0], "password": user[2]})
            reply = wire.recv(10)
            assert reply and reply.get("errno") == 6, reply
            assert process.poll() is None
            print("PASS Redis unavailable: explicit login error, server alive", flush=True)
        finally:
            sock.close()
    finally:
        stop_server(process)
        log.close()


def main():
    folder = Path(tempfile.mkdtemp(prefix="phase4-two-node-"))
    print(f"LOG_DIR={folder}", flush=True)
    a, log_a = start_server(folder, "A", 6000)
    b, log_b = start_server(folder, "B", 6001)
    try:
        for sender_protocol in ("json", "protobuf"):
            for receiver_protocol in ("json", "protobuf"):
                test_pair(sender_protocol + receiver_protocol,
                          sender_protocol, receiver_protocol)
        test_ack_loss()
        test_simultaneous_login()
        test_reconnect_and_stale_route()
        test_dead_node(b)
        test_redis_unavailable(folder)
        time.sleep(5.5)
        stop_server(a)
        for node, log in (("A", log_a), ("B", log_b)):
            log.flush()
            lines = Path(log.name).read_text().splitlines()
            metrics = [line for line in lines if "CLUSTER_METRICS" in line]
            delivery = [line for line in lines if "DELIVERY_METRICS" in line]
            print(f"METRICS {node}: {metrics[-1] if metrics else 'none'}", flush=True)
            print(f"DELIVERY {node}: {delivery[-1] if delivery else 'none'}", flush=True)
        print("PHASE4_TWO_NODE=PASS", flush=True)
    finally:
        stop_server(a)
        stop_server(b)
        log_a.close()
        log_b.close()


if __name__ == "__main__":
    main()
