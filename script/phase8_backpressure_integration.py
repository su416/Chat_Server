#!/usr/bin/env python3
"""Real-socket Phase 8 DB/slow-client/Stream overload fault cases."""

import argparse
import json
import os
from pathlib import Path
import re
import socket
import subprocess
import tempfile
import time

from bench_common import register_user
import phase4_two_node_integration as h
from reliable_delivery_integration import Wire, ONE_CHAT_MSG_ACK


def counters(path, prefix):
    lines = [line for line in path.read_text(errors="replace").splitlines()
             if prefix in line]
    if not lines:
        return {}
    values = {}
    for line in lines:
        for key, value in re.findall(r"([a-z_]+)=([0-9]+)", line):
            values[key] = max(values.get(key, 0), int(value))
    return values


def mysql_count(user, state):
    config = json.loads((h.ROOT / "conf/chatserver.json").read_text())["mysql"]
    env = {**os.environ, "MYSQL_PWD": config["password"]}
    query = ("SELECT COUNT(*) FROM pending_delivery WHERE receiver_user_id="
             f"{user} AND state={state}")
    result = subprocess.check_output([
        "mysql", "--batch", "--skip-column-names", "--host", config["host"],
        "--port", str(config["port"]), "--user", config["user"],
        "--database", config["dbname"], "--execute", query],
        env=env, text=True)
    return int(result.strip())


def wait_until(predicate, timeout, label):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            return
        time.sleep(0.2)
    raise AssertionError(f"timed out waiting for {label}")


def db_case(folder):
    process, log = h.start_server(folder, "P8DB", 6000,
        extra_env={"CHAT_PHASE8_TEST_DB_WORKER_DELAY_MS": "100"},
        flow={"db_worker_threads": 1, "db_max_queue_size": 8})
    sock = None
    try:
        sender = register_user("p8db", f"s{os.getpid()}")
        receiver = register_user("p8db", f"r{os.getpid()}")
        sock, wire = h.client(sender, 6000, "json")
        for index in range(80):
            h.send_one(wire, sender, receiver, f"phase8_db_{index}")
        rejected = 0
        deadline = time.monotonic() + 12
        while time.monotonic() < deadline:
            reply = wire.recv(0.2)
            if reply and reply.get("msgid") == ONE_CHAT_MSG_ACK and reply.get("errno"):
                rejected += 1
        assert rejected > 0, "expected explicit overload response"
        snapshot = counters(folder / "P8DB.log", "DELIVERY_METRICS")
        assert snapshot.get("db_queue_peak", 0) <= 8, snapshot
        assert snapshot.get("db_tasks_rejected", 0) > 0, snapshot
        # Stop while another bounded batch is queued; no indefinite drain.
        for index in range(12):
            h.send_one(wire, sender, receiver, f"phase8_shutdown_{index}")
        begin = time.monotonic()
        h.stop_server(process)
        elapsed = time.monotonic() - begin
        assert process.returncode != -9 and elapsed < 12, (process.returncode, elapsed)
        return {"rejected_responses": rejected, "queue_peak": snapshot["db_queue_peak"],
                "queue_capacity": 8, "shutdown_seconds": round(elapsed, 2)}
    finally:
        if sock:
            sock.close()
        h.stop_server(process)
        log.close()


def slow_case(folder):
    process, log = h.start_server(folder, "P8SLOW", 6000,
        flow={"output_high_watermark_bytes": 131072,
              "output_hard_limit_bytes": 262144})
    sockets = []
    try:
        key = f"p8slow_{os.getpid()}"
        sender = register_user(key, "sender")
        slow = register_user(key, "slow")
        fast_sender = register_user(key, "fasts")
        fast_receiver = register_user(key, "fastr")
        send_sock, send_wire = h.client(sender, 6000, "json")
        sockets.append(send_sock)
        payload = "x" * 65536
        for index in range(80):
            h.send_one(send_wire, sender, slow, f"p8slow_{index}_{payload}")
        wait_until(lambda: mysql_count(slow[0], 1) >= 80, 25, "durable offline rows")

        slow_sock = socket.create_connection(("127.0.0.1", 6000), timeout=5)
        slow_sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
        sockets.append(slow_sock)
        slow_wire = Wire(slow_sock, "json")
        slow_wire.send({"msgid": 1, "id": slow[0], "password": slow[2]})
        assert slow_wire.recv(10).get("errno") == 0
        # Intentionally never read from slow_sock again.
        fsock, fwire = h.client(fast_sender, 6000, "json")
        rsock, rwire = h.client(fast_receiver, 6000, "json")
        sockets.extend([fsock, rsock])
        begin = time.monotonic()
        h.send_one(fwire, fast_sender, fast_receiver, "phase8_fast_unaffected")
        fast_reply = rwire.recv(5)
        assert fast_reply and fast_reply.get("msg") == "phase8_fast_unaffected", fast_reply
        h.ack(rwire, fast_reply)
        fast_ms = (time.monotonic() - begin) * 1000

        wait_until(lambda: counters(folder / "P8SLOW.log", "DELIVERY_METRICS")
                   .get("slow_consumer_disconnects", 0) >= 1, 20,
                   "slow-consumer disconnect")
        wait_until(lambda: mysql_count(slow[0], 1) > 0, 10,
                   "durable WAITING_SESSION after disconnect")
        snapshot = counters(folder / "P8SLOW.log", "DELIVERY_METRICS")
        assert snapshot.get("output_buffer_highwater", 0) >= 1, snapshot

        slow_sock.close()
        restored_sock, restored = h.client(slow, 6000, "json")
        sockets.append(restored_sock)
        seen = set()
        deadline = time.monotonic() + 65
        while len(seen) < 80 and time.monotonic() < deadline:
            reply = restored.recv(1)
            if reply and reply.get("msgid") == 6:
                seen.add(str(reply["delivery_id"]))
                h.ack(restored, reply)
        assert len(seen) == 80, f"recovered {len(seen)}/80"
        return {"highwater_events": snapshot["output_buffer_highwater"],
                "disconnects": snapshot["slow_consumer_disconnects"],
                "fast_client_latency_ms": round(fast_ms, 2), "recovered": len(seen)}
    finally:
        for sock in sockets:
            sock.close()
        h.stop_server(process)
        log.close()


def redis_case(folder):
    suffix = str(os.getpid())
    a_name, b_name = "P8A" + suffix, "P8B" + suffix
    flow = {"stream_soft_limit": 8}
    a, a_log = h.start_server(folder, a_name, 6000, flow=flow)
    b, b_log = h.start_server(folder, b_name, 6001, flow=flow,
        extra_env={"CHAT_PHASE8_TEST_ROUTE_CONSUMER_DELAY_MS": "1000"})
    sockets = []
    try:
        sender = register_user("p8redis", "s" + suffix)
        receiver = register_user("p8redis", "r" + suffix)
        ssock, swire = h.client(sender, 6000, "json")
        rsock, rwire = h.client(receiver, 6001, "json")
        sockets.extend([ssock, rsock])
        for index in range(24):
            h.send_one(swire, sender, receiver, f"p8route_{index}")
        rejected = 0
        deadline = time.monotonic() + 11
        while time.monotonic() < deadline:
            response = swire.recv(0.1)
            if response and response.get("msgid") == ONE_CHAT_MSG_ACK and response.get("errno"):
                rejected += 1
            received = rwire.recv(0.1)
            if received and received.get("msgid") == 6:
                h.ack(rwire, received)
        a_metrics = counters(folder / f"{a_name}.log", "CLUSTER_METRICS")
        b_metrics = counters(folder / f"{b_name}.log", "CLUSTER_METRICS")
        assert rejected > 0, "Stream soft limit did not reject new routes"
        assert a_metrics.get("stream_overload_rejected", 0) > 0, a_metrics
        assert a_metrics.get("target_stream_peak", 0) >= 8, a_metrics
        assert b_metrics.get("stream_peak", 0) > 0, b_metrics
        return {"rejected_routes": rejected,
                "target_stream_peak_exact": a_metrics["target_stream_peak"],
                "stream_peak_sampled": b_metrics["stream_peak"],
                "stream_pending_peak_sample": b_metrics.get("route_pending", 0),
                "stream_lag_peak_sample": b_metrics.get("stream_lag", 0)}
    finally:
        for sock in sockets:
            sock.close()
        h.stop_server(a)
        h.stop_server(b)
        a_log.close()
        b_log.close()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--case", choices=("db", "slow", "redis"), required=True)
    case = parser.parse_args().case
    folder = Path(tempfile.mkdtemp(prefix="phase8-overload-"))
    print(f"LOG_DIR={folder}", flush=True)
    result = {"db": db_case, "slow": slow_case, "redis": redis_case}[case](folder)
    print(json.dumps({"case": case, "result": result, "logs": str(folder)}, indent=2), flush=True)


if __name__ == "__main__":
    main()
