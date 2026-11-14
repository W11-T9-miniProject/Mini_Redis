"""서버 실행 후: python3 -m unittest discover -s tests/integration -v

서버 미실행은 실제 실패다. 이 파일은 서버를 자동 실행하지 않는다.
전용 테스트 서버/포트를 사용한다. MR_PORT 환경 변수로 변경 가능.
"""

from concurrent.futures import ThreadPoolExecutor
import os
import socket
import time
import unittest
import uuid

HOST = os.environ.get("MR_HOST", "127.0.0.1")
PORT = int(os.environ.get("MR_PORT", "6379"))


def encode(*args):
    parts = [arg if isinstance(arg, bytes) else str(arg).encode() for arg in args]
    return (f"*{len(parts)}\r\n".encode() +
            b"".join(f"${len(part)}\r\n".encode() + part + b"\r\n" for part in parts))


class WireClient:
    """학습용 구현과 독립된 테스트 oracle. 하나의 연결을 유지한다."""

    def __enter__(self):
        self.socket = socket.create_connection((HOST, PORT), timeout=2)
        self.stream = self.socket.makefile("rb")
        return self

    def __exit__(self, *args):
        self.stream.close()
        self.socket.close()

    def read(self):
        line = self.stream.readline(1024)
        if not line.endswith(b"\r\n"):
            raise AssertionError(f"bad reply line: {line!r}")
        prefix, payload = line[:1], line[1:-2]
        if prefix == b"+":
            return payload
        if prefix == b"-":
            raise AssertionError(f"server error: {payload!r}")
        if prefix == b":":
            return int(payload)
        if prefix == b"$":
            length = int(payload)
            if length == -1:
                return None
            if not 0 <= length <= 1024 * 1024:
                raise AssertionError("invalid bulk length")
            data = self.stream.read(length)
            if len(data) != length or self.stream.read(2) != b"\r\n":
                raise AssertionError("truncated bulk reply")
            return data
        raise AssertionError(f"unknown reply: {line!r}")

    def call(self, *args):
        self.socket.sendall(encode(*args))
        return self.read()


class ServerTests(unittest.TestCase):
    def setUp(self):
        self.key = f"test:{uuid.uuid4().hex}"

    def test_crud_and_binary(self):
        with WireClient() as client:
            try:
                self.assertEqual(client.call("PING"), b"PONG")
                self.assertIsNone(client.call("GET", self.key))
                self.assertEqual(client.call("SET", self.key, b"a\x00\r\nb"), b"OK")
                self.assertEqual(client.call("GET", self.key), b"a\x00\r\nb")
                self.assertEqual(client.call("DEL", self.key), 1)
                self.assertEqual(client.call("DEL", self.key), 0)
            finally:
                client.call("DEL", self.key)

    def test_fragmented_and_pipelined(self):
        with WireClient() as client:
            wire = encode("PING")
            for byte in wire:
                client.socket.sendall(bytes([byte]))
            self.assertEqual(client.read(), b"PONG")
            client.socket.sendall(encode("PING") + encode("PING"))
            self.assertEqual(client.read(), b"PONG")
            self.assertEqual(client.read(), b"PONG")

    def test_expiration(self):
        with WireClient() as client:
            try:
                self.assertEqual(client.call("SET", self.key, "v", "PX", 100), b"OK")
                # 정확한 시각 경계는 C fake-clock 테스트에서 검증한다.
                # 여기서는 느린 CI에서도 재시도할 수 있도록 결국 만료됨을 확인.
                deadline = time.monotonic() + 2
                while time.monotonic() < deadline:
                    if client.call("GET", self.key) is None:
                        break
                    time.sleep(0.02)
                else:
                    self.fail("key did not expire")
            finally:
                client.call("DEL", self.key)

    def test_multiple_clients(self):
        def worker(index):
            key = f"{self.key}:{index}"
            with WireClient() as client:
                try:
                    for value in range(20):
                        self.assertEqual(client.call("SET", key, value), b"OK")
                        self.assertEqual(client.call("GET", key), str(value).encode())
                finally:
                    client.call("DEL", key)
        with ThreadPoolExecutor(max_workers=8) as pool:
            list(pool.map(worker, range(8)))

    def test_invalid_command_keeps_connection(self):
        with WireClient() as client:
            for args in (("HELLO",), ("GET",), ("SET", self.key, "v", "PX", "0"),
                         ("EXPIRE", self.key, "9223372036854775808")):
                with self.assertRaises(AssertionError):
                    client.call(*args)
                self.assertEqual(client.call("PING"), b"PONG")

    def test_protocol_error_and_oversized_request(self):
        for wire in (b"not-resp\r\n", b"*1\r\n$1048577\r\n", b"*9\r\n"):
            with WireClient() as client:
                client.socket.sendall(wire)
                self.assertTrue(client.stream.readline(1024).startswith(b"-ERR "))
                self.assertEqual(client.stream.read(1), b"")
        with WireClient() as control:
            self.assertEqual(control.call("PING"), b"PONG")

    def test_pipeline_larger_than_input_limit(self):
        # 합계가 1 MiB를 넘어도 각각 작은 정상 프레임이면 나눠 처리해야 한다.
        with WireClient() as client:
            client.socket.settimeout(10)
            count = 76000
            client.socket.sendall(encode("PING") * count)
            for _ in range(count):
                self.assertEqual(client.read(), b"PONG")

    def test_half_close_flushes_reply(self):
        with WireClient() as client:
            client.socket.sendall(encode("PING"))
            client.socket.shutdown(socket.SHUT_WR)
            self.assertEqual(client.read(), b"PONG")
            self.assertEqual(client.stream.read(1), b"")

    def test_ttl_reset_and_immediate_expire(self):
        with WireClient() as client:
            try:
                self.assertEqual(client.call("SET", self.key, "v", "PX", 10000), b"OK")
                self.assertGreaterEqual(client.call("TTL", self.key), 0)
                self.assertEqual(client.call("SET", self.key, "new"), b"OK")
                self.assertEqual(client.call("TTL", self.key), -1)
                self.assertEqual(client.call("EXPIRE", self.key, 0), 1)
                self.assertEqual(client.call("TTL", self.key), -2)
                self.assertEqual(client.call("EXPIRE", self.key, 1), 0)
            finally:
                client.call("DEL", self.key)

    def test_slow_reader_and_disconnect_do_not_block_others(self):
        with WireClient() as client:
            try:
                payload = b"x" * (512 * 1024)
                self.assertEqual(client.call("SET", self.key, payload), b"OK")
                with socket.create_connection((HOST, PORT), timeout=2) as slow:
                    slow.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
                    slow.sendall(encode("GET", self.key) * 30)
                    # 이 연결은 응답을 읽지 않는다. 다른 연결은 계속 진행되어야 한다.
                    with WireClient() as control:
                        self.assertEqual(control.call("PING"), b"PONG")
                with WireClient() as control:
                    self.assertEqual(control.call("PING"), b"PONG")
            finally:
                client.call("DEL", self.key)

    # TCP는 여러 send를 합칠 수 있다. 모든 분할 경계는 C 파서 테스트도 확인한다.


if __name__ == "__main__":
    unittest.main()
