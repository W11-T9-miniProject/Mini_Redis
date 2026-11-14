"""C 실행 파일을 실행하지 않고 Python 프로토콜/캐시 코드를 검증한다."""

import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import MagicMock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "examples"))
import api
from mini_client import MiniRedis, MiniRedisError, encode, read_reply


class ProtocolTests(unittest.TestCase):
    def test_encode_binary(self):
        self.assertEqual(encode("SET", "", b"a\x00\r\n"),
                         b"*3\r\n$3\r\nSET\r\n$0\r\n\r\n$4\r\na\x00\r\n\r\n")

    def test_read_all_reply_types(self):
        for wire, expected in ((b"+OK\r\n", b"OK"), (b":-2\r\n", -2),
                               (b"$-1\r\n", None), (b"$0\r\n\r\n", b""),
                               (b"$4\r\na\x00\r\n\r\n", b"a\x00\r\n")):
            with self.subTest(wire=wire):
                self.assertEqual(read_reply(io.BytesIO(wire)), expected)
        with self.assertRaises(MiniRedisError):
            read_reply(io.BytesIO(b"-ERR unsupported\r\n"))

    def test_reject_bad_reply(self):
        for wire in (b"+incomplete", b"$5\r\nshort", b"$-2\r\n", b":abc\r\n", b"?\r\n"):
            with self.subTest(wire=wire), self.assertRaises(OSError):
                read_reply(io.BytesIO(wire))

    def test_execute_sends_and_reads(self):
        connection = MagicMock()
        connection.makefile.return_value.__enter__.return_value = io.BytesIO(b"+OK\r\n")
        context = MagicMock()
        context.__enter__.return_value = connection
        with patch("mini_client.socket.create_connection", return_value=context) as create:
            self.assertEqual(MiniRedis(port=7777).set("key", "value", 100), b"OK")
        create.assert_called_once_with(("127.0.0.1", 7777), timeout=0.5)
        connection.sendall.assert_called_once_with(encode("SET", "key", "value", "PX", 100))


class Cache:
    """소켓 대신 메모리를 사용하는 테스트용 캐시."""
    def __init__(self):
        self.values = {}
        self.broken = False

    def get(self, key):
        if self.broken:
            raise OSError("connection unavailable")
        return self.values.get(key)

    def set(self, key, value, ttl_ms):
        if self.broken:
            raise OSError("connection unavailable")
        if ttl_ms != 5000:
            raise AssertionError("unexpected TTL")
        self.values[key] = value

    def delete(self, key):
        if self.broken:
            raise OSError("connection unavailable")
        self.values.pop(key, None)


class CacheFlowTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.original_db = api.DB_PATH
        api.DB_PATH = Path(self.temp.name) / "products.db"
        with api.STATS_LOCK:
            for key in api.STATS:
                api.STATS[key] = 0
        api.init_db()
        self.cache = Cache()

    def tearDown(self):
        api.DB_PATH = self.original_db
        self.temp.cleanup()

    def test_miss_hit_and_db_count(self):
        product, source = api.read_cached(self.cache, 1)
        self.assertEqual(source, "MISS")
        self.assertEqual(api.read_cached(self.cache, 1), (product, "HIT"))
        self.assertEqual(api.STATS["db_reads"], 1)
        self.assertEqual(json.loads(self.cache.values["product:1"]), product)

    def test_commit_then_invalidate(self):
        api.read_cached(self.cache, 1)
        self.assertTrue(api.write_cached(self.cache, 1, "mouse"))
        self.assertNotIn("product:1", self.cache.values)
        self.assertEqual(api.read_cached(self.cache, 1), ({"id": 1, "name": "mouse"}, "MISS"))

    def test_failure_fallback_and_committed_write(self):
        self.cache.broken = True
        self.assertEqual(api.read_cached(self.cache, 1)[1], "BYPASS")
        self.assertTrue(api.write_cached(self.cache, 1, "updated"))
        self.assertEqual(api.read_product(1)["name"], "updated")
        self.assertEqual(api.STATS["invalidation_errors"], 1)

    def test_corruption_and_missing_product(self):
        for raw in (b"not-json", b"null", b'"string"', b'{"id":999,"name":"wrong"}'):
            self.cache.values["product:1"] = raw
            self.assertEqual(api.read_cached(self.cache, 1)[1], "MISS")
        self.assertEqual(api.read_cached(self.cache, 999), (None, "MISS"))
        self.assertNotIn("product:999", self.cache.values)


if __name__ == "__main__":
    unittest.main()
