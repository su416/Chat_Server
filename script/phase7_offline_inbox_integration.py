#!/usr/bin/env python3
"""Phase 7 real MySQL/Redis/two-node offline inbox and crash checks."""
import argparse
import json
import os
from pathlib import Path
import socket
import tempfile
import threading
import time

import phase4_two_node_integration as p
import phase6_durable_pending_integration as f


def row(user, marker):
    assert marker.replace("-", "").replace("_", "").isalnum()
    value = f.sql("SELECT delivery_id,state,IFNULL(owner_node,'NULL') "
                  "FROM pending_delivery WHERE receiver_user_id="
                  f"{int(user[0])} AND payload LIKE '%{marker}%' "
                  "ORDER BY delivery_id DESC LIMIT 1")
    if not value:
        return None
    delivery, state, owner = value.split("\t")
    return int(delivery), int(state), owner


def state(delivery_id):
    value = f.sql("SELECT state,IFNULL(owner_node,'NULL') FROM pending_delivery "
                  f"WHERE delivery_id={int(delivery_id)}")
    if not value:
        return None
    state_value, owner = value.split("\t")
    return int(state_value), owner


def pair(label):
    return (p.register_user(p.RUN_ID, label + "s"),
            p.register_user(p.RUN_ID, label + "r"))


def send_offline(sender, receiver, marker, protocol="json"):
    sock, wire = p.client(sender, 6000, protocol)
    p.send_one(wire, sender, receiver, marker)
    return sock, wire


def receive_and_ack(receiver, port, protocol, marker, expected_id):
    sock, wire = p.client(receiver, port, protocol)
    try:
        got = p.receive_one(wire, marker)
        assert int(got["delivery_id"]) == expected_id, got
        p.ack(wire, got)
        f.wait_for(lambda: state(expected_id) == (2, "NULL") or
                   (state(expected_id) and state(expected_id)[0] == 2),
                   "durable ACK")
        return got
    finally:
        sock.close()


def backlog(receiver, count):
    now = int(time.time() * 1000)
    marker = f"{p.RUN_ID}-backlog-{count}"
    values = []
    for i in range(count):
        payload = json.dumps({"msgid": 6, "id": 1, "toid": receiver[0],
                              "name": "load", "msg": f"{marker}-{i}",
                              "time": "2026-09-18 00:00:00"}, separators=(",", ":"))
        values.append(f"(NULL,NULL,{receiver[0]},6,'{payload}',0,{now},"
                      f"{now + 86400000},1,{now},{now})")
    for start in range(0, len(values), 100):
        f.sql("INSERT INTO pending_delivery(route_id,owner_node,receiver_user_id,"
              "message_type,payload,retry_count,next_retry_at_ms,expire_at_ms,"
              "state,created_at_ms,updated_at_ms) VALUES " +
              ",".join(values[start:start + 100]))
    first = int(f.sql("SELECT MIN(delivery_id) FROM pending_delivery "
                      f"WHERE receiver_user_id={receiver[0]} "
                      f"AND payload LIKE '%{marker}%'"))
    started = time.monotonic()
    sock, wire = p.client(receiver, 6001, "json")
    login_ms = round((time.monotonic() - started) * 1000, 1)
    ids = []
    first_ms = None
    try:
        for i in range(count):
            got = wire.recv(30)
            assert got and got.get("msgid") == 6, (i, got)
            assert got["msg"] == f"{marker}-{i}", (i, got)
            ids.append(int(got["delivery_id"]))
            if first_ms is None:
                first_ms = round((time.monotonic() - started) * 1000, 1)
            p.ack(wire, got)
        assert ids == list(range(first, first + count)), ids[:10]
        f.wait_for(lambda: int(f.sql("SELECT COUNT(*) FROM pending_delivery "
                     f"WHERE receiver_user_id={receiver[0]} AND state=2 "
                     f"AND payload LIKE '%{marker}%'")) == count,
                   "backlog ACK complete", 30)
        total_ms = round((time.monotonic() - started) * 1000, 1)
        print(f"PASS backlog={count} login_ms={login_ms} first_ms={first_ms} "
              f"all_acked_ms={total_ms}", flush=True)
    finally:
        sock.close()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--backlog", type=int, default=100)
    parser.add_argument("--extended", action="store_true")
    args = parser.parse_args()
    folder = Path(tempfile.mkdtemp(prefix="phase7-offline-"))
    print(f"LOG_DIR={folder}", flush=True)
    a, log_a = p.start_server(folder, "A", 6000)
    b, log_b = p.start_server(folder, "B", 6001)
    try:
        for protocol in ("json", "protobuf"):
            sender, receiver = pair("offline-" + protocol)
            marker = f"{p.RUN_ID}-offline-{protocol}"
            sender_sock, _ = send_offline(sender, receiver, marker)
            saved = f.wait_for(lambda: row(receiver, marker), "offline row")
            assert saved[1:] == (1, "NULL"), saved
            assert int(f.sql("SELECT COUNT(*) FROM chatmessage WHERE toid="
                             f"{receiver[0]} AND status=2")) == 0
            receive_and_ack(receiver, 6001, protocol, marker, saved[0])
            print(f"PASS offline -> B {protocol} ACK id={saved[0]}", flush=True)
            sender_sock.close()

        sender, receiver = pair("reconnect")
        marker = f"{p.RUN_ID}-reconnect"
        sender_sock, _ = send_offline(sender, receiver, marker)
        saved = f.wait_for(lambda: row(receiver, marker), "reconnect offline row")
        recv_sock, recv_wire = p.client(receiver, 6001, "json")
        first = p.receive_one(recv_wire, marker)
        assert int(first["delivery_id"]) == saved[0]
        recv_sock.close()
        f.wait_for(lambda: state(saved[0]) == (1, "NULL"), "disconnect waiting", 8)
        receive_and_ack(receiver, 6000, "protobuf", marker, saved[0])
        print("PASS disconnect B -> WAITING -> A same deliveryId", flush=True)
        sender_sock.close()

        sender, receiver = pair("simultaneous")
        marker = f"{p.RUN_ID}-simultaneous"
        sender_sock, _ = send_offline(sender, receiver, marker)
        saved = f.wait_for(lambda: row(receiver, marker), "simultaneous offline row")
        sockets = {}
        failures = []
        barrier = threading.Barrier(2)

        def login_at(node, port):
            try:
                barrier.wait(timeout=5)
                sockets[node] = p.client(receiver, port, "json")
            except Exception as error:
                failures.append(str(error))

        left = threading.Thread(target=login_at, args=("A", 6000))
        right = threading.Thread(target=login_at, args=("B", 6001))
        left.start(); right.start(); left.join(); right.join()
        assert not failures and len(sockets) == 2, failures
        try:
            owner = p.redis("GET", f"chat:presence:user:{receiver[0]}").split("|")[0]
            loser = "B" if owner == "A" else "A"
            got = p.receive_one(sockets[owner][1], marker)
            assert int(got["delivery_id"]) == saved[0]
            assert sockets[loser][1].recv(1) is None
            p.ack(sockets[owner][1], got)
            f.wait_for(lambda: state(saved[0]) and state(saved[0])[0] == 2,
                       "simultaneous ACK")
            print(f"PASS simultaneous login: final owner={owner}, one delivery", flush=True)
        finally:
            for sock, _ in sockets.values():
                sock.close()
            sender_sock.close()

        sender, receiver = pair("restart")
        marker = f"{p.RUN_ID}-restart"
        sender_sock, _ = send_offline(sender, receiver, marker)
        saved = f.wait_for(lambda: row(receiver, marker), "restart offline row")
        f.kill_process(b)
        log_b.close()
        b, log_b = p.start_server(folder, "B", 6001)
        receive_and_ack(receiver, 6001, "json", marker, saved[0])
        print("PASS SIGKILL while offline, B restart, same deliveryId", flush=True)
        sender_sock.close()

        sender, receiver = pair("legacy")
        marker = f"{p.RUN_ID}-legacy"
        payload = json.dumps({"msgid": 6, "id": sender[0], "toid": receiver[0],
                              "name": sender[1], "msg": marker,
                              "time": "2026-09-18 00:00:00"}, separators=(",", ":"))
        f.sql("INSERT INTO chatmessage(fromid,toid,groupid,msg,status) VALUES"
              f"({sender[0]},{receiver[0]},NULL,'{payload}',2)")
        old = int(f.sql("SELECT id FROM chatmessage WHERE toid="
                        f"{receiver[0]} AND msg LIKE '%{marker}%'"))
        sock, wire = p.client(receiver, 6001, "json")
        got = p.receive_one(wire, marker)
        durable = int(got["delivery_id"])
        assert f.sql("SELECT legacy_chatmessage_id FROM pending_delivery "
                     f"WHERE delivery_id={durable}") == str(old)
        assert f.sql(f"SELECT status FROM chatmessage WHERE id={old}") == "0"
        p.ack(wire, got)
        f.wait_for(lambda: state(durable) and state(durable)[0] == 2,
                   "legacy ACK")
        sock.close()
        sock, wire = p.client(receiver, 6001, "json")
        assert wire.recv(2) is None, "legacy message redelivered"
        assert f.sql("SELECT COUNT(*) FROM pending_delivery WHERE "
                     f"legacy_chatmessage_id={old}") == "1"
        sock.close()
        print("PASS legacy chatmessage idempotent migration", flush=True)

        owner = p.register_user(p.RUN_ID, "legacy-group-owner")
        member = p.register_user(p.RUN_ID, "legacy-group-member")
        owner_sock, owner_wire = p.client(owner, 6000, "json")
        member_sock, member_wire = p.client(member, 6000, "json")
        group_name = f"{p.RUN_ID}-compat-group"
        owner_wire.send({"msgid": 8, "id": owner[0], "groupname": group_name,
                         "groupdesc": "phase7 compatibility"})
        created = owner_wire.recv(5)
        assert created and created.get("msgid") == 16 and created.get("errno") == 0, created
        group_id = created["groupid"]
        member_wire.send({"msgid": 9, "id": member[0], "groupid": group_id})
        joined = member_wire.recv(5)
        assert joined and joined.get("msgid") == 17 and joined.get("errno") == 0, joined
        member_sock.close()
        f.wait_for(lambda: p.redis("GET", f"chat:presence:user:{member[0]}") == "",
                   "legacy group member offline")
        group_marker = f"{p.RUN_ID}-group-compat"
        owner_wire.send({"msgid": 10, "id": owner[0], "groupid": group_id,
                         "name": owner[1], "msg": group_marker,
                         "time": "2026-09-18 00:00:00"})
        f.wait_for(lambda: f.sql("SELECT COUNT(*) FROM chatmessage WHERE "
            f"toid={member[0]} AND status=2 AND msg LIKE '%{group_marker}%'") == "1",
                   "legacy group offline row")
        raw = socket.create_connection(("127.0.0.1", 6000), timeout=5)
        try:
            group_wire = p.Wire(raw, "json")
            group_wire.send({"msgid": 1, "id": member[0], "password": member[2]})
            login_reply = group_wire.recv(10)
            assert login_reply and login_reply.get("errno") == 0, login_reply
            assert len(login_reply.get("offlinemsg", [])) == 1, login_reply
            assert group_marker in login_reply["offlinemsg"][0]
            assert f.sql("SELECT COUNT(*) FROM chatmessage WHERE "
                f"toid={member[0]} AND status=2 AND msg LIKE '%{group_marker}%'") == "0"
        finally:
            raw.close()
            owner_sock.close()
        print("PASS legacy group LOGIN_ACK.offlinemsg compatibility", flush=True)

        # A Phase 5 route may reach its old target just after Presence
        # disappears. The target now durably accepts it as WAITING_SESSION.
        sender, receiver = pair("route-goes-offline")
        route_id = f"{p.RUN_ID}-route-offline"
        f.direct_route(sender, receiver, route_id)
        route_row = f.wait_for(lambda: f.row_for_route(route_id),
                               "offline routed row")
        assert route_row[1] == 1, route_row
        f.direct_route(sender, receiver, route_id)
        assert f.sql("SELECT COUNT(*) FROM pending_delivery WHERE route_id="
                     f"'{route_id}'") == "1"
        receive_and_ack(receiver, 6000, "json", route_id, route_row[0])
        print("PASS stale target route -> one WAITING row -> A same ID", flush=True)

        # A failed durable INSERT must not be reported as accepted and must
        # not silently fall back to the legacy status=2 path.
        reject, reject_log = p.start_server(
            folder, "C", 6002,
            extra_env={"CHAT_PHASE7_TEST_REJECT_OFFLINE_PERSIST": "1"})
        try:
            sender, receiver = pair("db-reject")
            sender_sock, sender_wire = p.client(sender, 6002, "json")
            marker = f"{p.RUN_ID}-db-reject"
            p.send_one(sender_wire, sender, receiver, marker)
            reply = sender_wire.recv(5)
            assert reply and reply.get("msgid") == 18 and reply.get("errno") == 4, reply
            assert row(receiver, marker) is None
            assert f.sql("SELECT COUNT(*) FROM chatmessage WHERE toid="
                         f"{receiver[0]} AND status=2") == "0"
            sender_sock.close()
            print("PASS simulated DB reject: explicit sender failure, no legacy fallback", flush=True)
        finally:
            p.stop_server(reject)
            reject_log.close()

        sender, receiver = pair("claim-reject")
        marker = f"{p.RUN_ID}-claim-reject"
        sender_sock, _ = send_offline(sender, receiver, marker)
        saved = f.wait_for(lambda: row(receiver, marker), "claim reject offline row")
        claim_node, claim_log = p.start_server(
            folder, "E", 6004,
            extra_env={"CHAT_PHASE7_TEST_REJECT_CLAIM": "1"})
        try:
            sock, wire = p.client(receiver, 6004, "json")
            assert wire.recv(1.5) is None, "DB-failed claim sent a delivery"
            assert state(saved[0]) == (1, "NULL")
            assert claim_node.poll() is None, "login claim failure killed gateway"
            sock.close()
        finally:
            p.stop_server(claim_node)
            claim_log.close()
        claim_node, claim_log = p.start_server(folder, "E", 6004)
        try:
            receive_and_ack(receiver, 6004, "json", marker, saved[0])
            print("PASS claim DB failure: login succeeds, WAITING retained, retry after restart", flush=True)
        finally:
            p.stop_server(claim_node)
            claim_log.close()
            sender_sock.close()

        capacity_node, capacity_log = p.start_server(
            folder, "D", 6003,
            extra_env={"CHAT_PHASE7_TEST_MAX_PENDING": "4"})
        try:
            receiver = p.register_user(p.RUN_ID, "capacity-r")
            marker = f"{p.RUN_ID}-capacity"
            now = int(time.time() * 1000)
            values = []
            for i in range(10):
                payload = json.dumps({"msgid": 6, "id": 1, "toid": receiver[0],
                                      "name": "load", "msg": f"{marker}-{i}",
                                      "time": "2026-09-18 00:00:00"},
                                     separators=(",", ":"))
                values.append(f"(NULL,NULL,{receiver[0]},6,'{payload}',0,{now},"
                              f"{now + 86400000},1,{now},{now})")
            f.sql("INSERT INTO pending_delivery(route_id,owner_node,receiver_user_id,"
                  "message_type,payload,retry_count,next_retry_at_ms,expire_at_ms,"
                  "state,created_at_ms,updated_at_ms) VALUES " + ",".join(values))
            sock, wire = p.client(receiver, 6003, "json")
            try:
                first_batch = [wire.recv(5) for _ in range(4)]
                assert [m["msg"] for m in first_batch] == [
                    f"{marker}-{i}" for i in range(4)]
                waiting = int(f.sql("SELECT COUNT(*) FROM pending_delivery WHERE "
                    f"receiver_user_id={receiver[0]} AND state=1"))
                assert waiting >= 6, waiting
                for got in first_batch:
                    p.ack(wire, got)
                for i in range(4, 10):
                    got = wire.recv(8)
                    assert got and got["msg"] == f"{marker}-{i}", (i, got)
                    p.ack(wire, got)
                print("PASS capacity=4: six stayed WAITING; ACK freed next batches", flush=True)
            finally:
                sock.close()
        finally:
            p.stop_server(capacity_node)
            capacity_log.close()

        if args.extended:
            sender, receiver = pair("ack-close")
            marker = f"{p.RUN_ID}-ack-close"
            sender_sock, _ = send_offline(sender, receiver, marker)
            saved = f.wait_for(lambda: row(receiver, marker), "ACK/close row")
            sock, wire = p.client(receiver, 6001, "json")
            got = p.receive_one(wire, marker)
            p.ack(wire, got)
            f.wait_for(lambda: state(saved[0]) and state(saved[0])[0] == 2,
                       "ACK before close")
            sock.close()
            time.sleep(2.5)
            assert state(saved[0])[0] == 2, "late close reverted ACKED"
            sender_sock.close()
            print("PASS ACKED terminal after late disconnect", flush=True)

            sender, receiver = pair("expired-waiting")
            marker = f"{p.RUN_ID}-expired-waiting"
            sender_sock, _ = send_offline(sender, receiver, marker)
            saved = f.wait_for(lambda: row(receiver, marker), "expired WAITING row")
            f.sql("UPDATE pending_delivery SET expire_at_ms=1 WHERE delivery_id="
                  f"{saved[0]}")
            sock, wire = p.client(receiver, 6001, "json")
            assert wire.recv(1) is None, "expired waiting row was delivered"
            f.wait_for(lambda: state(saved[0]) and state(saved[0])[0] == 3,
                       "expired waiting cleanup", 40)
            sock.close()
            sender_sock.close()
            print("PASS expired WAITING row not delivered; scanner made EXPIRED", flush=True)

        receiver = p.register_user(p.RUN_ID, "backlog-r")
        backlog(receiver, args.backlog)

        print("PHASE7_OFFLINE_INBOX_INTEGRATION=PASS", flush=True)
    finally:
        p.stop_server(a)
        p.stop_server(b)
        log_a.close()
        log_b.close()


if __name__ == "__main__":
    main()
