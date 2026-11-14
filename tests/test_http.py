"""HTTP integration tests against a real server with a temporary log."""
import concurrent.futures
import http.client
import json
import os
from pathlib import Path
import select
import socket
import subprocess
import tempfile
import threading
import unittest


class HttpTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="mini-redis-http-")
        cls.addClassCleanup(cls.temp.cleanup)
        cls.log = str(Path(cls.temp.name) / "test.aof")
        cls.stderr = open(Path(cls.temp.name) / "server.stderr", "w+")
        cls.addClassCleanup(cls.stderr.close)
        cls.process = None
        cls.addClassCleanup(cls.stop_server)
        cls.start_server()

    @classmethod
    def start_server(cls):
        binary = os.environ.get("MINI_REDIS_SERVER", "build/mini-redis-server")
        cls.process = subprocess.Popen(
            [binary, "0", cls.log], stdout=subprocess.PIPE,
            stderr=cls.stderr, text=True)
        if not select.select([cls.process.stdout], [], [], 10)[0]:
            raise AssertionError("server did not report a listening port")
        line = cls.process.stdout.readline().strip()
        if not line.startswith("LISTENING 127.0.0.1:"):
            raise AssertionError(f"HTTP server is not implemented/started: {line!r}")
        cls.port = int(line.rsplit(":", 1)[1])

    @classmethod
    def stop_server(cls):
        if cls.process is None:
            return
        if cls.process.poll() is None:
            cls.process.terminate()
            try:
                cls.process.wait(timeout=15)
            except subprocess.TimeoutExpired:
                cls.process.kill()
                cls.process.wait()
                raise AssertionError("server did not stop cleanly")
            if cls.process.returncode != 0:
                raise AssertionError(f"server exited with {cls.process.returncode}")
        cls.process.stdout.close()
        cls.stderr.flush()
        cls.stderr.seek(0)
        errors = cls.stderr.read()
        if "AddressSanitizer" in errors or "runtime error:" in errors:
            raise AssertionError(errors)

    @classmethod
    def request(cls, payload=None, *, raw=None, method="POST", path="/kv",
                content_type="application/json"):
        body = raw if raw is not None else json.dumps(payload).encode()
        connection = http.client.HTTPConnection("127.0.0.1", cls.port, timeout=15)
        try:
            connection.request(method, path, body=body,
                               headers={"Content-Type": content_type})
            response = connection.getresponse()
            return response.status, json.loads(response.read())
        finally:
            connection.close()

    def test_01_basic_operations(self):
        self.assertEqual(self.request({"op": "set", "key": "basic", "value": "철수"})[0], 200)
        code, body = self.request({"op": "get", "key": "basic"})
        self.assertEqual((code, body["value"]), (200, "철수"))
        self.assertEqual(self.request({"op": "set", "key": "basic", "value": ""})[0], 200)
        self.assertEqual(self.request({"op": "get", "key": "basic"})[1]["value"], "")
        self.assertEqual(self.request({"op": "delete", "key": "basic"})[0], 200)
        self.assertEqual(self.request({"op": "get", "key": "basic"})[0], 404)
        self.assertEqual(self.request({"op": "delete", "key": "basic"})[0], 404)

    def test_02_expiration_and_stale_ids(self):
        self.assertEqual(self.request({"op": "set", "key": "expired", "value": "old",
                                       "expires_at_ms": 1})[0], 200)
        self.assertEqual(self.request({"op": "get", "key": "expired"})[0], 404)
        self.assertEqual(self.request({"op": "set", "key": "expired", "value": "new",
                                       "nx": True, "ttl_ms": 20000})[0], 200)
        self.assertEqual(self.request({"op": "compare_delete", "key": "expired",
                                       "expected": "old"})[0], 409)
        self.assertEqual(self.request({"op": "compare_set", "key": "expired", "expected": "old",
                                       "value": "stolen", "expires_at_ms": 0})[0], 409)
        self.assertEqual(self.request({"op": "compare_set", "key": "expired", "expected": "new",
                                       "value": "confirmed", "expires_at_ms": 0})[0], 200)
        self.assertEqual(self.request({"op": "compare_delete", "key": "expired",
                                       "expected": "confirmed"})[0], 200)

    def test_03_invalid_requests(self):
        invalid = [b"{", b"{} trailing", b"[]", b'{"op":"get","key":"x","key":"y"}',
                   b'{"op":"get","key":"x\\u0000hidden"}', b'{}\x00',
                   b'{"op":"set","key":"x","value":"v","ttl_ms":1e999}',
                   b'{"op":"set","key":"raw-control","value":"\x01"}',
                   b'{"op":"set","key":"bad-utf8","value":"\xff"}',
                   b'{"op":"set","key":"x","value":"\xc0\xaf"}',
                   b'{"op":"set","key":"x","value":"\xe0\x80\xaf"}',
                   b'{"op":"set","key":"x","value":"\xed\xa0\x80"}',
                   b'{"op":"set","key":"x","value":"\xf0\x80\x80\xaf"}',
                   b'{"op":"set","key":"x","value":"\xf4\x90\x80\x80"}',
                   b'{"op":"set","key":"x","value":"\xe2\x82"}',
                   b'{"op":"set","key":"x","value":"v","ttl_ms":01}',
                   b'{"op":"set","key":"x","value":"v","ttl_ms":1.e2}']
        for raw in invalid:
            with self.subTest(raw=raw):
                self.assertEqual(self.request(raw=raw)[0], 400)
        invalid_objects = [
            {}, {"op": "unknown", "key": "x"}, {"op": "get", "key": 1},
            {"op": "get", "key": "x", "unexpected": 1},
            {"op": "set", "key": "x", "value": "v", "nx": "true"},
            {"op": "set", "key": "x", "value": "v", "ttl_ms": 1.5},
            {"op": "set", "key": "x", "value": "v", "ttl_ms": True},
            {"op": "set", "key": "x", "value": "v", "ttl_ms": -1},
            {"op": "set", "key": "x", "value": "v", "ttl_ms": 9007199254740992},
            {"op": "set", "key": "x", "value": "v", "ttl_ms": 1, "expires_at_ms": 1},
            {"op": "set", "key": "x", "value": "v", "nx": True, "expires_at_ms": 1},
            {"op": "compare_set", "key": "x", "expected": "v", "value": "new"},
        ]
        for payload in invalid_objects:
            with self.subTest(payload=payload):
                self.assertEqual(self.request(payload)[0], 400)
        # Literal backslash-u text is legal; only an actual escaped NUL is rejected.
        self.assertEqual(self.request({"op": "set", "key": "literal", "value": r"\u0000"})[0], 200)
        self.assertEqual(self.request({"op": "set", "key": "escaped-control", "value": "\x01"})[0], 200)
        self.assertEqual(self.request({"op": "get", "key": "escaped-control"})[1]["value"], "\x01")

    def test_04_routing_and_limits(self):
        self.assertEqual(self.request({}, method="GET", path="/health")[0], 200)
        self.assertEqual(self.request({}, method="GET")[0], 405)
        self.assertEqual(self.request({}, path="/missing")[0], 404)
        self.assertEqual(self.request({}, content_type="text/plain")[0], 415)
        self.assertEqual(self.request({}, content_type="application/json garbage")[0], 415)
        self.assertEqual(self.request(raw=b"x" * 65537)[0], 413)

    def test_05_concurrent_nx(self):
        start = threading.Barrier(100)
        def claim(index):
            start.wait(timeout=15)
            result = self.request({"op": "set", "key": "race", "value": f"claim:{index}",
                                   "nx": True, "ttl_ms": 60000})
            return index, result[0]
        with concurrent.futures.ThreadPoolExecutor(max_workers=100) as pool:
            results = list(pool.map(claim, range(100)))
        winners = [index for index, code in results if code == 200]
        self.assertEqual(len(winners), 1)
        self.assertEqual(sum(code == 409 for _, code in results), 99)
        self.assertEqual(self.request({"op": "get", "key": "race"})[1]["value"],
                         f"claim:{winners[0]}")

    def test_06_aborted_request(self):
        with socket.create_connection(("127.0.0.1", self.port), timeout=5) as connection:
            connection.sendall(b"POST /kv HTTP/1.1\r\nHost: localhost\r\n"
                               b"Content-Type: application/json\r\nContent-Length: 100\r\n\r\n{")
        self.assertEqual(self.request({}, method="GET", path="/health")[0], 200)
        # Stop gracefully here so sanitizer can inspect aborted-request cleanup.
        type(self).stop_server()
        type(self).start_server()

    def test_07_restart_recovery(self):
        self.assertEqual(self.request({"op": "set", "key": "durable", "value": "confirmed"})[0], 200)
        cls = type(self)
        cls.process.kill()
        cls.process.wait(timeout=10)
        cls.process.stdout.close()
        cls.start_server()
        self.assertEqual(self.request({"op": "get", "key": "durable"})[1]["value"], "confirmed")

    def test_08_chunked_upload(self):
        payload = json.dumps({"op": "set", "key": "chunked", "value": "정글🙂" * 1000},
                             ensure_ascii=False).encode()
        connection = http.client.HTTPConnection("127.0.0.1", self.port, timeout=15)
        try:
            chunks = (payload[i:i + 37] for i in range(0, len(payload), 37))
            connection.request("POST", "/kv", body=chunks, encode_chunked=True,
                               headers={"Content-Type": "application/json; charset=utf-8"})
            response = connection.getresponse()
            self.assertEqual(response.status, 200)
            response.read()
        finally:
            connection.close()
        self.assertEqual(self.request({"op": "get", "key": "chunked"})[1]["value"], "정글🙂" * 1000)


if __name__ == "__main__":
    unittest.main(verbosity=2)
