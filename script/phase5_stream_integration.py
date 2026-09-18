#!/usr/bin/env python3
"""Phase 5 real Redis/MySQL integration: two nodes, four protocols, crash/recovery."""

import json
import os
import signal
import socket
import subprocess
import time
from pathlib import Path
import phase4_two_node_integration as p


def inject_stale_route(sender, receiver, receiver_wire, old_wire):
    route_id = f"{p.RUN_ID}-stale-stream"
    body = {"version": 1, "route_id": route_id, "source_node": "A",
            "target_node": "B", "message_type": 6,
            "sender_user_id": sender[0], "receiver_user_id": receiver[0],
            "hops": 0,
            "payload": {"msgid": 6, "id": sender[0], "toid": receiver[0],
                        "msg": route_id, "name": sender[1]}}
    p.redis("XADD", "chat:route:stream:B", "*", "body", json.dumps(body))
    p.ack(receiver_wire, p.receive_one(receiver_wire, route_id))
    assert old_wire.recv(0.5) is None, "old Session got stale route"
    assert int(p.redis("XLEN", "chat:route:stream:B")) >= 1
    print("PASS stale Stream route rerouted once and old Session rejected", flush=True)


def reconnect_and_stale():
    sender = p.register_user(p.RUN_ID, "move-s")
    receiver = p.register_user(p.RUN_ID, "move-r")
    sender_sock, sender_wire = p.client(sender, 6000, "json")
    old_sock, old_wire = p.client(receiver, 6001, "json")
    new_sock, new_wire = p.client(receiver, 6000, "protobuf")
    try:
        assert p.redis("GET", f"chat:presence:user:{receiver[0]}").startswith("A|")
        marker = f"{p.RUN_ID}-move"
        p.send_one(sender_wire, sender, receiver, marker)
        p.ack(new_wire, p.receive_one(new_wire, marker))
        assert old_wire.recv(0.5) is None
        inject_stale_route(sender, receiver, new_wire, old_wire)
        old_sock.close()
        time.sleep(0.5)
        assert p.redis("GET", f"chat:presence:user:{receiver[0]}").startswith("A|")
        marker = f"{p.RUN_ID}-after-old-close"
        p.send_one(sender_wire, sender, receiver, marker)
        p.ack(new_wire, p.receive_one(new_wire, marker))
        print("PASS delayed old disconnect preserved A owner", flush=True)
    finally:
        new_sock.close()
        old_sock.close()
        sender_sock.close()


def stopped_node_recovery(folder, process_b):
    sender = p.register_user(p.RUN_ID, "recover-s")
    receiver = p.register_user(p.RUN_ID, "recover-r")
    sender_sock, sender_wire = p.client(sender, 6000, "json")
    receiver_sock, _ = p.client(receiver, 6001, "json")
    try:
        os.kill(process_b.pid, signal.SIGSTOP)
        marker = f"{p.RUN_ID}-durable-backlog"
        p.send_one(sender_wire, sender, receiver, marker)
        time.sleep(0.5)
        assert marker in p.redis("XREVRANGE", "chat:route:stream:B",
                                 "+", "-", "COUNT", "1")
        print("PASS XADD persisted while B consumer stopped", flush=True)
        process_b.kill()
        process_b.wait(timeout=5)
        receiver_sock.close()
        process_b, new_log = p.start_server(folder, "B", 6001)
        receiver_sock, receiver_wire = p.client(receiver, 6001, "protobuf")
        delivered = p.receive_one(receiver_wire, marker)
        p.ack(receiver_wire, delivered)
        print("PASS killed B restarted: unread Stream route delivered via new Session",
              flush=True)
        return process_b, new_log
    finally:
        receiver_sock.close()
        sender_sock.close()


def pending_crash_recovery(folder, process_b):
    """Reject target DB acceptance so the Stream route remains in PEL."""
    process_b.kill()
    process_b.wait(timeout=5)
    process_b, rejected_log = p.start_server(
        folder, "B", 6001,
        extra_env={"CHAT_PHASE7_TEST_REJECT_OFFLINE_PERSIST": "1"})
    sender = p.register_user(p.RUN_ID, "pel-s")
    receiver = p.register_user(p.RUN_ID, "pel-r")
    route_id = f"{p.RUN_ID}-pel-crash"
    body = {"version": 1, "route_id": route_id, "source_node": "A",
            "target_node": "B", "message_type": 6,
            "sender_user_id": sender[0], "receiver_user_id": receiver[0],
            "hops": 0,
            "payload": {"msgid": 6, "id": sender[0], "toid": receiver[0],
                        "msg": route_id, "name": sender[1]}}
    entry_id = p.redis("XADD", "chat:route:stream:B", "*", "body", json.dumps(body))
    deadline = time.monotonic() + 3
    while time.monotonic() < deadline:
        if entry_id in p.redis("XPENDING", "chat:route:stream:B",
                               "chat:route:group:B", entry_id, entry_id, "1"):
            break
        time.sleep(0.1)
    else:
        raise AssertionError("B did not read route into PEL")
    process_b.kill()
    process_b.wait(timeout=5)
    rejected_log.close()
    print("PASS kill -9 B after rejected durable acceptance: route remained in PEL",
          flush=True)
    process_b, new_log = p.start_server(folder, "B", 6001)
    receiver_sock, receiver_wire = p.client(receiver, 6001, "json")
    try:
        p.ack(receiver_wire, p.receive_one(receiver_wire, route_id))
        print("PASS restarted B XAUTOCLAIM recovered PEL route", flush=True)
    finally:
        receiver_sock.close()
    return process_b, new_log


def duplicate_route():
    sender = p.register_user(p.RUN_ID, "dup-s")
    receiver = p.register_user(p.RUN_ID, "dup-r")
    receiver_sock, receiver_wire = p.client(receiver, 6001, "json")
    route_id = f"{p.RUN_ID}-duplicate"
    body = {"version": 1, "route_id": route_id, "source_node": "A",
            "target_node": "B", "message_type": 6,
            "sender_user_id": sender[0], "receiver_user_id": receiver[0],
            "hops": 0,
            "payload": {"msgid": 6, "id": sender[0], "toid": receiver[0],
                        "msg": route_id, "name": sender[1]}}
    try:
        for _ in range(2):
            p.redis("XADD", "chat:route:stream:B", "*", "body", json.dumps(body))
        p.ack(receiver_wire, p.receive_one(receiver_wire, route_id))
        assert receiver_wire.recv(1.5) is None, "duplicate route delivered twice"
        print("PASS duplicate routeId: one client delivery, both Stream entries ACKed",
              flush=True)
    finally:
        receiver_sock.close()


def capture_duplicate_metric(folder):
    deadline = time.monotonic() + 7
    while time.monotonic() < deadline:
        metrics = [line for line in (folder / "B.log").read_text().splitlines()
                   if "CLUSTER_METRICS" in line]
        if any("route_duplicate=1" in line for line in metrics):
            print(f"METRICS B-before-restart: {metrics[-1]}", flush=True)
            return
        time.sleep(0.2)
    raise AssertionError("B did not report route_duplicate=1")


def redis_recovery(folder):
    # An isolated Redis instance avoids interrupting the shared VM service.
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        redis_port = probe.getsockname()[1]
    d, log_d = p.start_server(folder, "D", 6003, redis_port=redis_port)
    redis_process = None
    e = None
    log_e = None
    try:
        redis_process = subprocess.Popen(
            ["redis-server", "--bind", "127.0.0.1", "--port", str(redis_port),
             "--save", "", "--appendonly", "no"],
            cwd=folder, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            if subprocess.run(["redis-cli", "-p", str(redis_port), "PING"],
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode == 0:
                break
            time.sleep(0.1)
        else:
            raise AssertionError("isolated Redis failed to start")
        deadline = time.monotonic() + 8
        while time.monotonic() < deadline:
            node = subprocess.check_output(
                ["redis-cli", "-p", str(redis_port), "--raw", "GET", "chat:node:D"],
                text=True).strip()
            if node:
                break
            time.sleep(0.2)
        else:
            raise AssertionError("D did not re-register after Redis recovery")
        e, log_e = p.start_server(folder, "E", 6004, redis_port=redis_port)
        sender = p.register_user(p.RUN_ID, "redis-recover-s")
        receiver = p.register_user(p.RUN_ID, "redis-recover-r")
        sender_sock, sender_wire = p.client(sender, 6003, "json")
        receiver_sock, receiver_wire = p.client(receiver, 6004, "protobuf")
        try:
            marker = f"{p.RUN_ID}-redis-recovered"
            p.send_one(sender_wire, sender, receiver, marker)
            p.ack(receiver_wire, p.receive_one(receiver_wire, marker))
            print("PASS Redis recovered: existing D process reconnected, D->E routed",
                  flush=True)
        finally:
            receiver_sock.close()
            sender_sock.close()
    finally:
        p.stop_server(d)
        if e:
            p.stop_server(e)
        log_d.close()
        if log_e:
            log_e.close()
        if redis_process:
            redis_process.terminate()
            redis_process.wait(timeout=5)


def three_node_migration(folder, process_b):
    c, log_c = p.start_server(folder, "C", 6002)
    sender = p.register_user(p.RUN_ID, "migrate-s")
    receiver = p.register_user(p.RUN_ID, "migrate-r")
    sender_sock, sender_wire = p.client(sender, 6000, "json")
    old_sock, old_wire = p.client(receiver, 6001, "json")
    new_sock = None
    try:
        os.kill(process_b.pid, signal.SIGSTOP)
        marker = f"{p.RUN_ID}-migrate-C"
        p.send_one(sender_wire, sender, receiver, marker)
        assert marker in p.redis("XREVRANGE", "chat:route:stream:B",
                                 "+", "-", "COUNT", "1")
        new_sock, new_wire = p.client(receiver, 6002, "protobuf")
        assert p.redis("GET", f"chat:presence:user:{receiver[0]}").startswith("C|")
        os.kill(process_b.pid, signal.SIGCONT)
        p.ack(new_wire, p.receive_one(new_wire, marker))
        assert old_wire.recv(0.5) is None
        print("PASS three-node A->B Stream, owner migrated B->C, B rerouted to C",
              flush=True)
    finally:
        if process_b.poll() is None:
            os.kill(process_b.pid, signal.SIGCONT)
        if new_sock:
            new_sock.close()
        old_sock.close()
        sender_sock.close()
        p.stop_server(c)
        log_c.close()


def main():
    import tempfile
    folder = Path(tempfile.mkdtemp(prefix="phase5-stream-"))
    print(f"LOG_DIR={folder}", flush=True)
    a, log_a = p.start_server(folder, "A", 6000)
    b, log_b = p.start_server(folder, "B", 6001)
    try:
        for sender_protocol in ("json", "protobuf"):
            for receiver_protocol in ("json", "protobuf"):
                p.test_pair(sender_protocol + receiver_protocol,
                            sender_protocol, receiver_protocol)
        p.test_ack_loss()
        p.test_simultaneous_login()
        reconnect_and_stale()
        log_b.flush()
        log_b.close()
        b, log_b = stopped_node_recovery(folder, b)
        duplicate_route()
        capture_duplicate_metric(folder)
        three_node_migration(folder, b)
        log_b.flush()
        log_b.close()
        b, log_b = pending_crash_recovery(folder, b)
        p.test_redis_unavailable(folder)
        redis_recovery(folder)
        time.sleep(5.5)
        p.stop_server(a)
        p.stop_server(b)
        for node, log in (("A", log_a), ("B", log_b)):
            log.flush()
            metrics = [line for line in Path(log.name).read_text().splitlines()
                       if "CLUSTER_METRICS" in line]
            print(f"METRICS {node}: {metrics[-1] if metrics else 'none'}", flush=True)
        print("PHASE5_STREAM_INTEGRATION=PASS", flush=True)
    finally:
        p.stop_server(a)
        p.stop_server(b)
        log_a.close()
        log_b.close()


if __name__ == "__main__":
    main()
