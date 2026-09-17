#!/usr/bin/env python3
"""真实 socket 的 Phase 3 单节点可靠单聊验证与可复现故障注入。"""

import argparse
import json
import os
import socket
import struct
import threading
import time

from bench_common import compute_stats, register_user
from reliable_client import RecentDeliveryIds, handle_one_chat

try:
    import chat_pb2
except ImportError:
    chat_pb2 = None

HOST, PORT = "127.0.0.1", 6000


class Wire:
    def __init__(self, sock, protocol):
        self.sock = sock
        self.protocol = "json"
        self.buffer = bytearray()
        if protocol == "protobuf":
            if chat_pb2 is None:
                raise RuntimeError("generate chat_pb2.py and set PYTHONPATH")
            self.send({"msgid": 0, "accept": "protobuf"})
            response = self.recv(5)
            if not response or response.get("status") != "ok":
                raise RuntimeError(f"protobuf negotiation failed: {response}")
            self.protocol = "protobuf"

    def send(self, obj, typed_ack=False):
        if self.protocol == "json":
            self.sock.sendall(json.dumps(obj).encode() + b"\0")
        else:
            envelope = chat_pb2.Envelope(msgid=obj["msgid"])
            if typed_ack:
                envelope.payload = chat_pb2.MessageAck(
                    delivery_id=int(obj["delivery_id"])).SerializeToString()
            else:
                envelope.payload = json.dumps(obj).encode()
            body = envelope.SerializeToString()
            self.sock.sendall(struct.pack("!I", len(body)) + body)

    def recv(self, timeout):
        deadline = time.monotonic() + timeout
        while True:
            if self.protocol == "json":
                marker = self.buffer.find(b"\0")
                if marker >= 0:
                    body = bytes(self.buffer[:marker])
                    del self.buffer[:marker + 1]
                    return json.loads(body)
            elif len(self.buffer) >= 4:
                length = struct.unpack("!I", self.buffer[:4])[0]
                if length > 1024 * 1024:
                    raise RuntimeError("oversized protobuf response")
                if len(self.buffer) >= 4 + length:
                    body = bytes(self.buffer[4:4 + length])
                    del self.buffer[:4 + length]
                    envelope = chat_pb2.Envelope()
                    envelope.ParseFromString(body)
                    return json.loads(envelope.payload)

            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return None
            self.sock.settimeout(remaining)
            try:
                chunk = self.sock.recv(4096)
            except socket.timeout:
                return None
            if not chunk:
                return None
            self.buffer.extend(chunk)


def connect_and_login(user, protocol):
    sock = socket.create_connection((HOST, PORT), timeout=5)
    try:
        wire = Wire(sock, protocol)
        wire.send({"msgid": 1, "id": user[0], "password": user[2]})
        response = wire.recv(10)
        if not response or response.get("errno") != 0:
            raise RuntimeError(f"login failed for {user[0]}: {response}")
        return sock, wire
    except Exception:
        sock.close()
        raise


def run(args):
    run_id = f"phase3_{int(time.time())}_{os.getpid()}"
    pairs = []
    for index in range(args.senders):
        sender = register_user(run_id, f"s{index}")
        receiver = register_user(run_id, f"r{index}")
        pairs.append((sender, receiver))

    receiver_connections = [connect_and_login(receiver, args.protocol)
                            for _, receiver in pairs]
    sender_connections = [connect_and_login(sender, args.protocol)
                          for sender, _ in pairs]

    sent_stamps = {}
    received_stamps = {}
    unique_count = [0] * args.senders
    duplicate_count = [0] * args.senders
    ack_count = [0] * args.senders
    drop_count = [0] * args.senders
    errors = []
    lock = threading.Lock()
    stop = threading.Event()
    barrier = threading.Barrier(args.senders)

    def receiver_task(index):
        _, wire = receiver_connections[index]
        recent = RecentDeliveryIds(4096)
        ack_attempts = {}
        while not stop.is_set():
            try:
                message = wire.recv(0.25)
                if message is None:
                    continue
                if message.get("msgid") != 6:
                    continue
                delivery_id = message.get("delivery_id")
                if not delivery_id:
                    raise RuntimeError("online delivery lacks delivery_id")

                def process(payload):
                    with lock:
                        unique_count[index] += 1
                        received_stamps[payload["msg"]] = time.perf_counter()

                def acknowledge(ack):
                    attempt = ack_attempts.get(delivery_id, 0)
                    ack_attempts[delivery_id] = attempt + 1
                    drop = args.drop_ack == "all" or (
                        args.drop_ack == "first10" and attempt == 0
                        and (unique_count[index] - 1) % 10 == 0)
                    if drop:
                        drop_count[index] += 1
                        return
                    wire.send(ack, typed_ack=args.typed_ack)
                    ack_count[index] += 1

                first = handle_one_chat(message, recent, process, acknowledge)
                if not first:
                    duplicate_count[index] += 1
            except Exception as error:
                errors.append(f"receiver {index}: {error}")
                return

    def sender_task(index):
        sender, receiver = pairs[index]
        _, wire = sender_connections[index]
        try:
            barrier.wait(timeout=10)
            for sequence in range(args.messages):
                marker = f"{run_id}_{index}_{sequence}"
                request = {"msgid": 6, "id": sender[0], "toid": receiver[0],
                           "name": sender[1], "msg": marker,
                           "time": "2026-09-17 00:00:00"}
                with lock:
                    sent_stamps[marker] = time.perf_counter()
                wire.send(request)
        except Exception as error:
            errors.append(f"sender {index}: {error}")

    receivers = [threading.Thread(target=receiver_task, args=(i,), daemon=True)
                 for i in range(args.senders)]
    senders = [threading.Thread(target=sender_task, args=(i,))
               for i in range(args.senders)]
    start = time.perf_counter()
    for thread in receivers + senders:
        thread.start()
    for thread in senders:
        thread.join(timeout=15)
    if any(thread.is_alive() for thread in senders):
        errors.append("sender thread did not finish")

    deadline = time.monotonic() + (8.0 if args.drop_ack == "all" else
                                   3.5 if args.drop_ack == "first10" else 0.7)
    while time.monotonic() < deadline:
        time.sleep(0.1)
    stop.set()
    for thread in receivers:
        thread.join(timeout=1)
    elapsed = time.perf_counter() - start

    # 保持已登录连接一小段时间，让服务端心跳输出最终指标；断线清理
    # 在当前基线有偶发阻塞，不能把断线后的旧快照当成最终计数。
    time.sleep(args.linger)

    for sock, _ in receiver_connections + sender_connections:
        sock.close()

    latencies = [(received_stamps[key] - stamp) * 1000
                 for key, stamp in sent_stamps.items() if key in received_stamps]
    result = {
        "protocol": args.protocol,
        "senders": args.senders,
        "messages_per_sender": args.messages,
        "drop_ack": args.drop_ack,
        "sent": len(sent_stamps),
        "delivered_unique": sum(unique_count),
        "acked_wire_frames": sum(ack_count),
        "retry_duplicate_frames": sum(duplicate_count),
        "duplicate_suppressed": sum(duplicate_count),
        "ack_dropped": sum(drop_count),
        "failed_client_side": len(sent_stamps) - sum(unique_count),
        "elapsed_seconds": round(elapsed, 3),
        "qps_unique": round(sum(unique_count) / elapsed, 1),
        "latency": compute_stats(latencies),
        "errors": errors,
    }
    print(json.dumps(result, indent=2), flush=True)
    expected = args.senders * args.messages
    if errors or result["sent"] != expected or result["delivered_unique"] != expected:
        raise RuntimeError("reliable delivery integration failed")
    if args.drop_ack == "all" and result["acked_wire_frames"] != 0:
        raise RuntimeError("100% ACK loss scenario sent ACK")
    if args.drop_ack != "none" and result["retry_duplicate_frames"] == 0:
        raise RuntimeError("missing expected retry")
    return result


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--protocol", choices=("json", "protobuf"), default="json")
    parser.add_argument("--senders", type=int, default=1)
    parser.add_argument("--messages", type=int, default=5)
    parser.add_argument("--drop-ack", choices=("none", "first10", "all"), default="none")
    parser.add_argument("--typed-ack", action="store_true")
    parser.add_argument("--linger", type=float, default=2.5)
    run(parser.parse_args())
