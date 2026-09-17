import unittest

from script.reliable_client import RecentDeliveryIds, handle_one_chat


class ReliableClientTest(unittest.TestCase):
    def test_duplicate_is_suppressed_but_acknowledged_twice(self):
        recent = RecentDeliveryIds(2)
        handled = []
        acks = []
        message = {"msgid": 6, "delivery_id": "1001", "msg": "hello"}

        self.assertTrue(handle_one_chat(message, recent, handled.append, acks.append))
        self.assertFalse(handle_one_chat(message, recent, handled.append, acks.append))
        self.assertEqual(len(handled), 1)
        self.assertEqual(acks, [{"msgid": 20, "delivery_id": "1001"}] * 2)

    def test_cache_is_bounded_and_old_messages_are_evicted(self):
        recent = RecentDeliveryIds(2)
        self.assertTrue(recent.observe("1"))
        self.assertTrue(recent.observe("2"))
        self.assertTrue(recent.observe("3"))
        self.assertEqual(len(recent.ids), 2)
        self.assertTrue(recent.observe("1"))

    def test_no_ack_if_business_handler_fails(self):
        acks = []
        with self.assertRaises(RuntimeError):
            handle_one_chat(
                {"msgid": 6, "delivery_id": "2"}, RecentDeliveryIds(),
                lambda _: (_ for _ in ()).throw(RuntimeError("handler failed")),
                acks.append,
            )
        self.assertEqual(acks, [])

    def test_invalid_id_is_rejected(self):
        for value in (0, "0", "-1", "abc", 1.1):
            with self.subTest(value=value), self.assertRaises(ValueError):
                handle_one_chat(
                    {"msgid": 6, "delivery_id": value}, RecentDeliveryIds(),
                    lambda _: None, lambda _: None,
                )


if __name__ == "__main__":
    unittest.main()
