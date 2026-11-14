"""C 서버의 RESP API를 Python 함수로 감싼 작은 클라이언트.

호출마다 연결을 생성한다. 구현이 단순하고 스레드 간 소켓 공유가 없지만
TCP 연결 비용이 발생하므로 벤치마크 해석에 포함해야 한다.
"""

import argparse
import socket

MAX_REPLY_BYTES = 2 * 1024 * 1024


class MiniRedisError(Exception):
    """서버의 -ERR 응답. 연결 오류(OSError)와 구분한다."""


def encode(*args):
    parts = []
    for arg in args:
        if isinstance(arg, bytes):
            parts.append(arg)
        elif isinstance(arg, (str, int)):
            parts.append(str(arg).encode("utf-8"))
        else:
            raise TypeError("RESP arguments must be str, int, or bytes")
    return (f"*{len(parts)}\r\n".encode() +
            b"".join(f"${len(part)}\r\n".encode() + part + b"\r\n" for part in parts))


def read_reply(stream):
    # 헤더만 줄 단위로 읽는다. 값 자체는 길이를 기준으로 읽어야 NUL/개행도 보존한다.
    line = stream.readline(128)
    if not line.endswith(b"\r\n"):
        raise OSError("truncated or oversized RESP header")
    prefix, text = line[:1], line[1:-2]
    if prefix == b"+":
        return text
    if prefix == b"-":
        raise MiniRedisError(text.decode("utf-8", errors="replace"))
    try:
        if prefix == b":":
            return int(text)
        if prefix == b"$":
            length = int(text)
            if length == -1:
                return None
            if not 0 <= length <= MAX_REPLY_BYTES:
                raise OSError("invalid RESP bulk length")
            payload = stream.read(length)
            if len(payload) != length or stream.read(2) != b"\r\n":
                raise OSError("truncated RESP bulk")
            return payload
    except ValueError as exc:
        raise OSError("invalid RESP number") from exc
    raise OSError("unsupported RESP reply")


class MiniRedis:
    def __init__(self, host="127.0.0.1", port=6379, timeout=0.5):
        self.host = host
        self.port = port
        self.timeout = timeout

    def execute(self, *args):
        # with를 중첩하면 정상/오류/timeout 경로 모두 stream과 socket을 정리한다.
        wire = encode(*args)
        with socket.create_connection((self.host, self.port), timeout=self.timeout) as connection:
            connection.sendall(wire)
            with connection.makefile("rb") as stream:
                return read_reply(stream)

    def get(self, key):
        return self.execute("GET", key)

    def set(self, key, value, ttl_ms=5000):
        if ttl_ms is None:
            return self.execute("SET", key, value)
        return self.execute("SET", key, value, "PX", ttl_ms)

    def delete(self, key):
        return self.execute("DEL", key)


def main():
    parser = argparse.ArgumentParser(description="Mini Redis CLI (RESP2)")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=6379)
    parser.add_argument("command", nargs="+", help="e.g. SET name kim")
    args = parser.parse_args()
    try:
        result = MiniRedis(args.host, args.port).execute(*args.command)
    except (OSError, MiniRedisError) as exc:
        parser.exit(1, f"Error: {exc}\n")
    if result is None:
        print("(nil)")
    elif isinstance(result, bytes):
        print(result.decode("utf-8", errors="backslashreplace"))
    else:
        print(result)


if __name__ == "__main__":
    main()
