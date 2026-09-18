#!/usr/bin/env python3
"""Real two-node Phase 6 fault tests. Requires Redis, MySQL and chat_pb2."""

import json
import os
from pathlib import Path
import re
import signal
import subprocess
import tempfile
import time

import phase4_two_node_integration as p
from reliable_client import RecentDeliveryIds


CONFIG = json.loads((p.ROOT / "conf/chatserver.json").read_text())["mysql"]
MYSQL_ENV = {**os.environ, "MYSQL_PWD": CONFIG["password"]}
MYSQL = ["mysql", "--batch", "--skip-column-names", "--host", CONFIG["host"],
         "--port", str(CONFIG["port"]), "--user", CONFIG["user"],
         "--database", CONFIG["dbname"]]


def sql(statement):
    result = subprocess.run(MYSQL + ["--execute", statement], env=MYSQL_ENV,
                            text=True, capture_output=True, check=True)
    return result.stdout.strip()


def wait_for(predicate, label, seconds=8):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        value = predicate()
        if value:
            return value
        time.sleep(0.1)
    raise AssertionError(f"timed out: {label}")


def row_for_route(route):
    # Test-generated route IDs contain only letters, digits, '_' and '-'.
    assert route.replace("-", "").replace("_", "").isalnum()
    result = sql("SELECT delivery_id,state,retry_count FROM pending_delivery "
                 f"WHERE route_id='{route}'")
    return tuple(map(int, result.split("\t"))) if result else None


def row_for_id(delivery_id):
    result = sql("SELECT delivery_id,state,retry_count FROM pending_delivery "
                 f"WHERE delivery_id={int(delivery_id)}")
    return tuple(map(int, result.split("\t"))) if result else None


def kill_process(process):
    if process.poll() is None:
        process.kill()  # SIGKILL, not graceful shutdown.
        process.wait(timeout=5)


def new_pair(label):
    return (p.register_user(p.RUN_ID, label + "s"),
            p.register_user(p.RUN_ID, label + "r"))


def send(sender_wire, sender, receiver, marker):
    p.send_one(sender_wire, sender, receiver, marker)


def direct_route(sender, receiver, route):
    body = {"version": 1, "route_id": route, "source_node": "A",
            "target_node": "B", "message_type": 6,
            "sender_user_id": sender[0], "receiver_user_id": receiver[0],
            "hops": 0,
            "payload": {"msgid": 6, "id": sender[0], "toid": receiver[0],
                        "msg": route, "name": sender[1]}}
    return p.redis("XADD", "chat:route:stream:B", "*", "body", json.dumps(body))


def main():
    folder = Path(tempfile.mkdtemp(prefix="phase6-durable-"))
    print(f"LOG_DIR={folder}", flush=True)
    a, log_a = p.start_server(folder, "A", 6000)
    b = None
    log_b = None
    restart_number = 0

    def restart_b(extra_env=None):
        nonlocal b, log_b, restart_number
        if b:
            kill_process(b)
            log_b.flush()
            log_b.close()
            Path(log_b.name).rename(folder / f"B-before-restart-{restart_number}.log")
            restart_number += 1
        b, log_b = p.start_server(folder, "B", 6001,
                                  extra_env=extra_env)

    try:
        # Case 1: the DB worker stops the process immediately after INSERT,
        # before it can queue client send or let the Stream consumer XACK.
        restart_b({"CHAT_PHASE6_TEST_STOP_AFTER_PERSIST": "1"})
        sender, receiver = new_pair("presend")
        sender_sock, sender_wire = p.client(sender, 6000, "json")
        receiver_sock, receiver_wire = p.client(receiver, 6001, "protobuf")
        marker = f"{p.RUN_ID}-persist-before-send"
        send(sender_wire, sender, receiver, marker)
        def matching_route():
            latest = p.redis("XREVRANGE", "chat:route:stream:B",
                             "+", "-", "COUNT", "1")
            return latest if marker in latest else None
        route = wait_for(matching_route, "matching route entry")
        route_id = route.splitlines()[0]
        durable = wait_for(lambda: sql(
            "SELECT delivery_id FROM pending_delivery WHERE receiver_user_id="
            f"{receiver[0]} AND state=0 ORDER BY delivery_id DESC LIMIT 1"),
            "durable INSERT")
        assert receiver_wire.recv(0.4) is None, "sent before controlled crash"
        assert route_id in p.redis("XPENDING", "chat:route:stream:B",
                                    "chat:route:group:B", route_id, route_id, "1")
        kill_process(b)
        receiver_sock.close()
        recovery_start = time.monotonic()
        restart_b()
        receiver_sock, receiver_wire = p.client(receiver, 6001, "protobuf")
        recovered = p.receive_one(receiver_wire, marker)
        recovery_ms = round((time.monotonic() - recovery_start) * 1000, 1)
        assert int(recovered["delivery_id"]) == int(durable)
        p.ack(receiver_wire, recovered)
        wait_for(lambda: row_for_id(durable)[1] == 2, "ACK durable")
        log_b.flush()
        counts = re.findall(r"DURABLE_STARTUP_RECOVERY node=B pending=(\d+)",
                            Path(log_b.name).read_text())
        assert counts and int(counts[-1]) >= 1, counts
        print("PASS case1 persisted-before-send SIGKILL and restart: "
              f"same deliveryId; recovery_ms={recovery_ms}; "
              f"startup_count={counts[-1]}", flush=True)
        receiver_sock.close()
        sender_sock.close()

        # Case 2/3: client receives, ACK is lost, target dies; client dedups
        # the same ID after restart. Existing Phase 3 retry still uses that ID.
        sender, receiver = new_pair("ackloss")
        sender_sock, sender_wire = p.client(sender, 6000, "json")
        receiver_sock, receiver_wire = p.client(receiver, 6001, "json")
        marker = f"{p.RUN_ID}-send-before-ack"
        send(sender_wire, sender, receiver, marker)
        first = p.receive_one(receiver_wire, marker)
        dedup = RecentDeliveryIds(4096)
        assert dedup.observe(first["delivery_id"])
        assert row_for_id(first["delivery_id"])[1] == 0
        kill_process(b)
        receiver_sock.close()
        recovery_start = time.monotonic()
        restart_b()
        receiver_sock, receiver_wire = p.client(receiver, 6001, "json")
        again = p.receive_one(receiver_wire, marker)
        recovery_ms = round((time.monotonic() - recovery_start) * 1000, 1)
        assert again["delivery_id"] == first["delivery_id"]
        assert not dedup.observe(again["delivery_id"])
        p.ack(receiver_wire, again)
        wait_for(lambda: row_for_id(first["delivery_id"])[1] == 2,
                 "lost ACK recovery")
        print("PASS cases2/3 send-before-ACK SIGKILL: "
              f"same ID; client dedup; recovery_ms={recovery_ms}", flush=True)
        receiver_sock.close()
        sender_sock.close()

        # Case 4: durable ACK is the completion barrier.
        sender, receiver = new_pair("acked")
        sender_sock, sender_wire = p.client(sender, 6000, "protobuf")
        receiver_sock, receiver_wire = p.client(receiver, 6001, "json")
        marker = f"{p.RUN_ID}-acked-before-crash"
        send(sender_wire, sender, receiver, marker)
        delivered = p.receive_one(receiver_wire, marker)
        p.ack(receiver_wire, delivered)
        wait_for(lambda: row_for_id(delivered["delivery_id"])[1] == 2,
                 "durable ACK commit")
        kill_process(b)
        receiver_sock.close()
        restart_b()
        receiver_sock, receiver_wire = p.client(receiver, 6001, "json")
        assert receiver_wire.recv(2.5) is None, "ACKED row replayed"
        print("PASS case4 ACK committed before SIGKILL: no replay", flush=True)
        receiver_sock.close()
        sender_sock.close()

        # Case 5: inject DB persist rejection without disrupting the VM's
        # shared MySQL service. The Stream entry must remain in the PEL.
        restart_b({"CHAT_PHASE6_TEST_REJECT_PERSIST": "1"})
        sender, receiver = new_pair("dbfail")
        sender_sock, sender_wire = p.client(sender, 6000, "json")
        receiver_sock, receiver_wire = p.client(receiver, 6001, "json")
        route = f"{p.RUN_ID}-db-unavailable"
        entry = direct_route(sender, receiver, route)
        wait_for(lambda: entry in p.redis("XPENDING", "chat:route:stream:B",
                                           "chat:route:group:B", entry, entry, "1"),
                 "unacknowledged Stream PEL", 3)
        assert row_for_route(route) is None
        kill_process(b)
        receiver_sock.close()
        restart_b()
        receiver_sock, receiver_wire = p.client(receiver, 6001, "json")
        replay = p.receive_one(receiver_wire, route)
        p.ack(receiver_wire, replay)
        wait_for(lambda: row_for_route(route)[1] == 2, "DB recovery ACK")
        print("PASS case5 simulated DB write failure: no XACK, PEL replay", flush=True)
        receiver_sock.close()
        sender_sock.close()

        # Case 6: repeated routeId must map to one durable delivery row.
        sender, receiver = new_pair("duplicate")
        receiver_sock, receiver_wire = p.client(receiver, 6001, "json")
        route = f"{p.RUN_ID}-duplicate-durable"
        direct_route(sender, receiver, route)
        first = p.receive_one(receiver_wire, route)
        direct_route(sender, receiver, route)
        p.ack(receiver_wire, first)
        wait_for(lambda: row_for_route(route)[1] == 2, "duplicate route ACK")
        assert int(sql("SELECT COUNT(*) FROM pending_delivery "
                       f"WHERE route_id='{route}'")) == 1
        assert receiver_wire.recv(1.5) is None
        print("PASS case6 duplicate route/claim: one durable row and ID", flush=True)
        receiver_sock.close()

        # Case 7: disconnected receiver enters WAITING_SESSION, then login
        # reclaims the durable row instead of a 500ms hot retry loop.
        sender, receiver = new_pair("offline")
        sender_sock, sender_wire = p.client(sender, 6000, "json")
        receiver_sock, receiver_wire = p.client(receiver, 6001, "json")
        marker = f"{p.RUN_ID}-waiting-session"
        send(sender_wire, sender, receiver, marker)
        first = p.receive_one(receiver_wire, marker)
        receiver_sock.close()
        wait_for(lambda: row_for_id(first["delivery_id"])[1] == 1,
                 "WAITING_SESSION", 6)
        receiver_sock, receiver_wire = p.client(receiver, 6001, "json")
        resumed = p.receive_one(receiver_wire, marker)
        assert resumed["delivery_id"] == first["delivery_id"]
        p.ack(receiver_wire, resumed)
        wait_for(lambda: row_for_id(first["delivery_id"])[1] == 2,
                 "session resumed ACK")
        print("PASS case7 offline WAITING_SESSION and login recovery", flush=True)
        receiver_sock.close()
        sender_sock.close()

        # A durable row may outlive its original owner node. The new owner
        # claims it at login and retains the same client delivery identity.
        sender, receiver = new_pair("migration")
        sender_sock, sender_wire = p.client(sender, 6000, "json")
        receiver_sock, receiver_wire = p.client(receiver, 6001, "json")
        marker = f"{p.RUN_ID}-durable-owner-migration"
        send(sender_wire, sender, receiver, marker)
        first = p.receive_one(receiver_wire, marker)
        receiver_sock.close()
        wait_for(lambda: row_for_id(first["delivery_id"])[1] == 1,
                 "owner migration WAITING_SESSION", 6)
        receiver_sock, receiver_wire = p.client(receiver, 6000, "protobuf")
        moved = p.receive_one(receiver_wire, marker)
        assert moved["delivery_id"] == first["delivery_id"]
        p.ack(receiver_wire, moved)
        wait_for(lambda: row_for_id(first["delivery_id"])[1] == 2,
                 "owner migration ACK")
        print("PASS case7b durable pending owner migration B->A; same ID", flush=True)
        receiver_sock.close()
        sender_sock.close()

        # Case 8: test-owned row is set expired, then startup recovery must
        # terminalize it and never replay it to a new Session.
        sender, receiver = new_pair("expired")
        sender_sock, sender_wire = p.client(sender, 6000, "json")
        receiver_sock, receiver_wire = p.client(receiver, 6001, "json")
        marker = f"{p.RUN_ID}-expired-pending"
        send(sender_wire, sender, receiver, marker)
        first = p.receive_one(receiver_wire, marker)
        sql("UPDATE pending_delivery SET expire_at_ms=1 WHERE delivery_id="
            f"{int(first['delivery_id'])}")
        kill_process(b)
        receiver_sock.close()
        restart_b()
        receiver_sock, receiver_wire = p.client(receiver, 6001, "json")
        wait_for(lambda: row_for_id(first["delivery_id"])[1] == 3,
                 "expired startup row")
        assert receiver_wire.recv(2.5) is None
        print("PASS case8 expired row terminalized on restart", flush=True)
        receiver_sock.close()
        sender_sock.close()

        print("PHASE6_DURABLE_PENDING_INTEGRATION=PASS", flush=True)
    finally:
        if b:
            kill_process(b)
        p.stop_server(a)
        if log_b:
            log_b.close()
        log_a.close()


if __name__ == "__main__":
    main()
