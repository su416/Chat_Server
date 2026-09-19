#!/usr/bin/env python3
"""Small JSON demo against already-running nodes on ports 6000 and 6001."""

import os
import time

from bench_common import register_user
import phase4_two_node_integration as cluster


def main():
    run = f"demo_{int(time.time())}_{os.getpid()}"
    sender = register_user(run, "sender")
    receiver = register_user(run, "receiver")
    sender_sock, sender_wire = cluster.client(sender, 6000, "json")
    receiver_sock, receiver_wire = cluster.client(receiver, 6001, "json")
    try:
        marker = f"{run}_hello"
        cluster.send_one(sender_wire, sender, receiver, marker)
        delivery = cluster.receive_one(receiver_wire, marker)
        cluster.ack(receiver_wire, delivery)
        print(f"PASS node-a -> Redis Stream -> node-b delivery_id={delivery['delivery_id']}")
    finally:
        sender_sock.close()
        receiver_sock.close()


if __name__ == "__main__":
    main()
