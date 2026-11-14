"""실제 C 서버 + 별도 HTTP 프로세스 + 임시 SQLite로 cache-aside를 검증한다."""

import json
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time
import unittest
from urllib.error import HTTPError, URLError
from urllib.request import Request, urlopen

from test_server import HOST, PORT, WireClient

ROOT = Path(__file__).resolve().parents[2]


def unused_port():
    with socket.socket() as connection:
        connection.bind(("127.0.0.1", 0))
        return connection.getsockname()[1]


def http(port, path="/products/1", method="GET", data=None):
    body = None if data is None else json.dumps(data).encode()
    request = Request(f"http://127.0.0.1:{port}{path}", data=body, method=method,
                      headers={"Content-Type": "application/json"})
    try:
        response = urlopen(request, timeout=3)
    except HTTPError as error:
        response = error
    with response:
        return response.status, json.loads(response.read()), response.headers.get("X-Cache")


class ApiTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        # 동일한 전용 Mini Redis를 쓴다. 실제 서비스에 실행하지 않는다.
        if HOST != "127.0.0.1":
            raise RuntimeError("API integration requires a local dedicated server")
        cls.temp = tempfile.TemporaryDirectory()
        cls.processes = []
        cls.addClassCleanup(cls.cleanup)
        for mode in ("on", "off", "unavailable"):
            port = unused_port()
            redis_port = unused_port() if mode == "unavailable" else PORT
            process = subprocess.Popen(
                [sys.executable, str(ROOT / "examples/api.py"), "--port", str(port),
                 "--cache", "off" if mode == "off" else "on", "--redis-port", str(redis_port),
                 "--db", str(Path(cls.temp.name) / f"{mode}.db")],
                stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
            cls.processes.append(process)
            setattr(cls, f"port_{mode}", port)
            deadline = time.monotonic() + 8
            while True:
                try:
                    if http(port, "/stats")[0] == 200:
                        break
                except (URLError, OSError):
                    pass
                if process.poll() is not None or time.monotonic() >= deadline:
                    raise RuntimeError("HTTP API did not start")
                time.sleep(0.05)

    @classmethod
    def cleanup(cls):
        for process in cls.processes:
            process.terminate()
            try:
                process.communicate(timeout=3)
            except subprocess.TimeoutExpired:
                process.kill()
                process.communicate()
        cls.temp.cleanup()

    def setUp(self):
        with WireClient() as client:
            client.call("DEL", "product:1")

    def tearDown(self):
        with WireClient() as client:
            client.call("DEL", "product:1")

    def test_miss_hit_invalidation_and_db_count(self):
        before = http(self.port_on, "/stats")[1]["db_reads"]
        status, first, cache = http(self.port_on)
        self.assertEqual((status, cache), (200, "MISS"))
        self.assertEqual(http(self.port_on), (200, first, "HIT"))
        self.assertEqual(http(self.port_on, "/stats")[1]["db_reads"] - before, 1)
        self.assertEqual(http(self.port_on, method="PUT", data={"name": "mouse"})[0], 200)
        status, updated, cache = http(self.port_on)
        self.assertEqual((status, updated["name"], cache), (200, "mouse", "MISS"))
        self.assertEqual(http(self.port_on)[2], "HIT")

    def test_cache_expiration(self):
        self.assertEqual(http(self.port_on)[2], "MISS")
        time.sleep(5.1)
        self.assertEqual(http(self.port_on)[2], "MISS")

    def test_corrupted_cache(self):
        with WireClient() as client:
            client.call("SET", "product:1", b"not-json")
        self.assertEqual(http(self.port_on)[2], "MISS")
        self.assertEqual(http(self.port_on)[2], "HIT")

    def test_unavailable_cache_falls_back(self):
        self.assertEqual(http(self.port_unavailable)[0::2], (200, "BYPASS"))
        self.assertEqual(http(self.port_unavailable, method="PUT", data={"name": "fallback"})[0], 200)
        self.assertEqual(http(self.port_unavailable)[1]["name"], "fallback")
        self.assertGreater(http(self.port_unavailable, "/stats")[1]["invalidation_errors"], 0)

    def test_baseline_and_invalid_inputs(self):
        self.assertEqual(http(self.port_off)[0::2], (200, "BYPASS"))
        self.assertEqual(http(self.port_off, "/products/999")[0], 404)
        self.assertEqual(http(self.port_off, method="PUT", data={"name": 123})[0], 400)
        self.assertEqual(http(self.port_off, "/products/999999999999999999999999999")[0], 404)


if __name__ == "__main__":
    unittest.main()
