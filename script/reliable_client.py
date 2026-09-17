"""Phase 3 JSON/Protobuf 测试客户端的业务层去重与 ACK 辅助逻辑。"""

from collections import deque

MESSAGE_ACK = 20
ONE_CHAT_MSG = 6
MAX_DELIVERY_ID = (1 << 64) - 1


class RecentDeliveryIds:
    def __init__(self, capacity=4096):
        if capacity <= 0:
            raise ValueError("capacity must be positive")
        self.capacity = capacity
        self.ids = set()
        self.order = deque()

    def observe(self, delivery_id):
        """新 ID 返回 True；重复 ID 返回 False。存储容量始终有界。"""
        if delivery_id in self.ids:
            return False
        if len(self.order) >= self.capacity:
            self.ids.remove(self.order.popleft())
        self.ids.add(delivery_id)
        self.order.append(delivery_id)
        return True

    def contains(self, delivery_id):
        return delivery_id in self.ids


def handle_one_chat(message, recent_ids, on_message, send_ack):
    """业务处理完成后 ACK；重复消息不再处理，但始终重发 ACK。"""
    if message.get("msgid") != ONE_CHAT_MSG:
        return False

    delivery_id = message.get("delivery_id")
    if delivery_id is None:
        on_message(message)  # 兼容旧服务端消息，没有可靠投递契约。
        return True
    if (not isinstance(delivery_id, str) or not delivery_id.isdecimal()
            or not 0 < int(delivery_id) <= MAX_DELIVERY_ID):
        raise ValueError("invalid delivery_id")

    first = not recent_ids.contains(delivery_id)
    if first:
        on_message(message)
        recent_ids.observe(delivery_id)
    send_ack({"msgid": MESSAGE_ACK, "delivery_id": delivery_id})
    return first
