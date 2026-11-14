"""SQLite 상품 API. GET → 캐시 → DB, PUT → DB 커밋 → 캐시 삭제.

실행: python3 examples/api.py --port 8080 --cache off
      python3 examples/api.py --port 8081 --cache on
"""

import argparse
from contextlib import contextmanager
import json
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import sqlite3
import threading
from urllib.parse import urlparse

from mini_client import MiniRedis, MiniRedisError

DB_PATH = Path(__file__).resolve().parent / "products.db"

# HTTP 서버는 여러 스레드를 쓰므로 통계 카운터도 lock으로 보호한다.
# C 저장소는 단일 이벤트 루프, Python API는 멀티스레드라는 차이에 주목한다.
STATS = {"db_reads": 0, "hits": 0, "misses": 0, "bypasses": 0, "invalidation_errors": 0}
STATS_LOCK = threading.Lock()


def count(name):
    with STATS_LOCK:
        STATS[name] += 1


@contextmanager
def database():
    # sqlite3.Connection의 with는 커밋/롤백만 담당하고 연결을 닫지는 않는다.
    # finally에서 close해야 Windows 파일 잠금과 요청마다 쌓이는 연결을 방지한다.
    connection = sqlite3.connect(DB_PATH)
    try:
        with connection:
            yield connection
    finally:
        connection.close()


def init_db():
    with database() as db:
        db.execute("CREATE TABLE IF NOT EXISTS products (id INTEGER PRIMARY KEY, name TEXT NOT NULL)")
        db.execute("INSERT OR IGNORE INTO products VALUES (1, 'keyboard')")


def read_product(product_id):
    count("db_reads")
    with database() as db:
        row = db.execute("SELECT id, name FROM products WHERE id = ?", (product_id,)).fetchone()
    return None if row is None else {"id": row[0], "name": row[1]}


def write_product(product_id, name):
    with database() as db:
        cursor = db.execute("UPDATE products SET name = ? WHERE id = ?", (name, product_id))
    return cursor.rowcount > 0


def read_cached(client, product_id):
    key = f"product:{product_id}"
    try:
        raw = client.get(key)
        if raw is not None:
            try:
                product = json.loads(raw)
                if (isinstance(product, dict) and product.get("id") == product_id
                        and isinstance(product.get("name"), str)):
                    count("hits")
                    return product, "HIT"
            except (ValueError, UnicodeError):
                pass  # 잘못된 캐시 내용은 원본 DB로 다시 채운다.
    except (OSError, MiniRedisError):
        count("bypasses")
        return read_product(product_id), "BYPASS"
    product = read_product(product_id)
    if product is not None:
        try:
            client.set(key, json.dumps(product).encode("utf-8"), ttl_ms=5000)
        except (OSError, MiniRedisError):
            # DB 응답은 이미 확보했다. 캐시 저장 실패 때문에 조회를 실패시키지 않는다.
            count("bypasses")
            return product, "BYPASS"
    count("misses")
    return product, "MISS"


def write_cached(client, product_id, name):
    # DB가 원본이다. 커밋 전에 캐시만 지우면 그 사이 옛 DB 값이 다시 캐시될 수 있다.
    # 커밋 후 DEL도 강한 일관성을 보장하지는 않는다. TTL과 함께 사용하는 예제다.
    found = write_product(product_id, name)
    if found:
        try:
            client.delete(f"product:{product_id}")
        except (OSError, MiniRedisError):
            count("invalidation_errors")  # DB 변경은 성공. 최대 TTL 동안 오래된 값 가능.
    return found


def main():
    global DB_PATH
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, default=8080)
    parser.add_argument("--cache", choices=("on", "off"), default="off")
    parser.add_argument("--redis-port", type=int, default=6379)
    parser.add_argument("--db", type=Path, default=DB_PATH)
    args = parser.parse_args()
    DB_PATH = args.db
    init_db()
    client = MiniRedis(port=args.redis_port)

    class Handler(BaseHTTPRequestHandler):
        def send_json(self, status, data, cache="BYPASS"):
            body = json.dumps(data).encode("utf-8")
            self.send_response(status)
            self.send_header("Content-Type", "application/json; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.send_header("X-Cache", cache)
            self.end_headers()
            self.wfile.write(body)

        def product_id(self):
            parts = urlparse(self.path).path.strip("/").split("/")
            if len(parts) != 2 or parts[0] != "products" or not parts[1].isdigit():
                return None
            if len(parts[1]) > 18:
                return None  # SQLite INTEGER 범위/거대한 정수 변환 제한.
            return int(parts[1])

        def do_GET(self):
            if urlparse(self.path).path == "/stats":
                with STATS_LOCK:
                    snapshot = dict(STATS)
                return self.send_json(200, snapshot)
            product_id = self.product_id()
            if product_id is None:
                return self.send_json(404, {"error": "not found"})
            if args.cache == "on":
                product, source = read_cached(client, product_id)
            else:
                product, source = read_product(product_id), "BYPASS"
                count("bypasses")
            self.send_json(200 if product else 404, product, source)

        def do_PUT(self):
            product_id = self.product_id()
            if product_id is None:
                return self.send_json(404, {"error": "not found"})
            try:
                length = int(self.headers.get("Content-Length", "0"))
                if not 0 < length <= 4096:
                    return self.send_json(400, {"error": "body length must be 1..4096"})
                data = json.loads(self.rfile.read(length))
                if not isinstance(data, dict) or not isinstance(data.get("name"), str):
                    return self.send_json(400, {"error": "name must be a string"})
                if args.cache == "on":
                    found = write_cached(client, product_id, data["name"])
                else:
                    found = write_product(product_id, data["name"])
                self.send_json(200 if found else 404, {"updated": found})
            except (ValueError, UnicodeError):
                self.send_json(400, {"error": "invalid JSON or length"})

        def log_message(self, format_string, *values):
            pass  # 벤치마크 중 요청마다 콘솔 I/O가 발생하지 않도록 한다.

    server = ThreadingHTTPServer(("127.0.0.1", args.port), Handler)
    print(f"API http://127.0.0.1:{args.port}/products/1 cache={args.cache}", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
