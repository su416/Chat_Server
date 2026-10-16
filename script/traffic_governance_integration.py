#!/usr/bin/env python3
"""Real sockets/Redis/MySQL: layered quotas, ACK bypass and Stream recovery."""
import json
import os
from pathlib import Path
import tempfile
import time

import phase4_two_node_integration as h
from phase6_durable_pending_integration import row_for_id, sql, wait_for
from reliable_delivery_integration import ONE_CHAT_MSG_ACK
from phase8_backpressure_integration import counters


def limiter_case(folder, global_limit):
    label = "global" if global_limit else "user"
    rate = {"enabled": True, "user_capacity": 20 if global_limit else 1,
            "user_rate_per_sec": 0.001, "global_capacity": 1 if global_limit else 20,
            "global_rate_per_sec": 0.001}
    process, log = h.start_server(folder, "traffic-" + label, 6000, rate_limit=rate)
    sockets = []
    try:
        users = [h.register_user(h.RUN_ID, label + str(i)) for i in range(3)]
        wires = []
        for user in users:
            sock, wire = h.client(user, 6000, "json")
            sockets.append(sock)
            wires.append(wire)
        h.send_one(wires[0], users[0], users[2], "first-" + label)
        delivery = h.receive_one(wires[2], "first-" + label)
        # Exhaust either the same user's bucket or the node bucket with another user.
        index = 1 if global_limit else 0
        h.send_one(wires[index], users[index], users[2], "rejected-" + label)
        reply = wires[index].recv(3)
        reason = "SERVER_OVERLOADED" if global_limit else "RATE_LIMITED"
        assert reply and reply.get("msgid") == ONE_CHAT_MSG_ACK and reply.get("reason") == reason, reply
        assert reply.get("retryable") is True, reply
        if global_limit:
            repeated = h.receive_one(wires[2], "first-" + label)
            assert repeated["delivery_id"] == delivery["delivery_id"], repeated
        # Both buckets remain exhausted when the real client ACK arrives.
        h.ack(wires[2], delivery)
        wait_for(lambda: row_for_id(delivery["delivery_id"])[1] == 2,
                 "durable ACK bypasses " + label)
        if not global_limit:
            h.send_one(wires[1], users[1], users[2], "independent-user")
            second = h.receive_one(wires[2], "independent-user")
            h.ack(wires[2], second)
            wait_for(lambda: row_for_id(second["delivery_id"])[1] == 2, "second durable ACK")
        # GROUP_CHAT shares the same user/global admission as ONE_CHAT.
        wires[index].send({"msgid": 10, "id": users[index][0], "groupid": 1,
                           "msg": "rejected group", "name": users[index][1]})
        group_reply = wires[index].recv(3)
        assert group_reply and group_reply.get("msgid") == 19 and group_reply.get("reason") == reason, group_reply
        metric_name = f"rate_limit_{label}_rejected"
        snapshot = wait_for(lambda: (values if (values := counters(
            folder / ("traffic-" + label + ".log"), "CLUSTER_METRICS"))
            .get(metric_name, 0) == 2 else None), "admission metrics")
        # Metrics and heartbeat both run every five seconds on different threads.
        # The first metrics snapshot can precede the first heartbeat completion.
        wait_for(lambda: counters(folder / ("traffic-" + label + ".log"),
                                  "CLUSTER_METRICS").get("node_heartbeat", 0) > 0,
                 "heartbeat completes while business quotas remain exhausted")
        print(f"PASS {label}: explicit {reason}; GROUP_CHAT covered; durable ACK bypass; "
              f"{metric_name}={snapshot[metric_name]}", flush=True)
    finally:
        for sock in sockets:
            sock.close()
        h.stop_server(process)
        log.close()


def stream_case(folder):
    node = "traffic-target-" + str(os.getpid())
    a, la = h.start_server(folder, "traffic-source", 6000, flow={"stream_soft_limit": 2})
    b, lb = h.start_server(folder, node, 6001)
    sockets = []
    try:
        sender = h.register_user(h.RUN_ID, "stream-s")
        receiver = h.register_user(h.RUN_ID, "stream-r")
        ss, sw = h.client(sender, 6000, "json")
        rs, rw = h.client(receiver, 6001, "json")
        sockets += [ss, rs]
        key = "chat:route:stream:" + node
        # Pause only the target process; Presence lease remains valid during this short test.
        import signal
        b.send_signal(signal.SIGSTOP)
        marker = h.RUN_ID + "-stream-"
        h.send_one(sw, sender, receiver, marker + "1")
        h.send_one(sw, sender, receiver, marker + "2")
        wait_for(lambda: int(h.redis("XLEN", key)) == 2, "watermark reached", seconds=3)
        h.send_one(sw, sender, receiver, marker + "rejected")
        reply = sw.recv(3)
        assert reply and reply.get("reason") == "TARGET_OVERLOADED" and reply.get("errno") == 4, reply
        assert int(h.redis("XLEN", key)) == 2
        b.send_signal(signal.SIGCONT)
        ids = []
        for index in (1, 2):
            delivery = h.receive_one(rw, marker + str(index))
            ids.append(delivery["delivery_id"])
            h.ack(rw, delivery)
        wait_for(lambda: int(h.redis("XLEN", key)) < 2, "safe trim restores headroom")
        h.send_one(sw, sender, receiver, marker + "recovered")
        recovered = h.receive_one(rw, marker + "recovered")
        ids.append(recovered["delivery_id"])
        h.ack(rw, recovered)
        wait_for(lambda: all(row_for_id(id)[1] == 2 for id in ids), "all accepted ACKED")
        rejected_rows = sql("SELECT COUNT(*) FROM pending_delivery WHERE receiver_user_id="
                            f"{receiver[0]} AND JSON_UNQUOTE(JSON_EXTRACT(payload,'$.msg'))='{marker}rejected'")
        assert rejected_rows == "0", rejected_rows
        snapshot = wait_for(lambda: (values if (values := counters(
            folder / "traffic-source.log", "CLUSTER_METRICS"))
            .get("route_backpressure_rejected", 0) == 1 else None), "Stream metrics")
        print("PASS Stream: 2 accepted, 1 TARGET_OVERLOADED, safe trim then recovery; "
              f"3 durable ACKED; route_backpressure_rejected={snapshot['route_backpressure_rejected']}", flush=True)
    finally:
        import signal
        if b.poll() is None:
            b.send_signal(signal.SIGCONT)
        for sock in sockets:
            sock.close()
        h.stop_server(a)
        h.stop_server(b)
        la.close()
        lb.close()


def main():
    folder = Path(tempfile.mkdtemp(prefix="chat-traffic-"))
    print(f"LOG_DIR={folder}", flush=True)
    limiter_case(folder, False)
    limiter_case(folder, True)
    stream_case(folder)
    print("TRAFFIC_GOVERNANCE=PASS", flush=True)


if __name__ == "__main__":
    main()
