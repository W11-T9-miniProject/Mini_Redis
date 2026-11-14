"""틈: 로컬 정적 웹 + SQLite 자동완성 + C Mini Redis를 연결하는 HTTP 서버.

실행: python examples/search_app.py --port 8090
흐름: 브라우저 → /api/suggest → Mini Redis GET → MISS면 SQLite 집계 → SET PX.
응답의 X-Cache와 /api/stats는 개발자 측정용이며 웹 UI에 노출하지 않는다.
"""

import argparse
from collections import Counter
from contextlib import contextmanager
import hashlib
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
import random
import sqlite3
import threading
import time
import unicodedata
from urllib.parse import parse_qs, unquote, urlsplit

from mini_client import MiniRedis, MiniRedisError
from search_data import CATEGORIES, PLACES, STARTERS

ROOT = Path(__file__).resolve().parents[1]
WEB_ROOT = ROOT / "web" / "teum"
INITIALS = "ㄱㄲㄴㄷㄸㄹㅁㅂㅃㅅㅆㅇㅈㅉㅊㅋㅌㅍㅎ"
DISTRICT_ALIASES = {"성수동": "성수", "연남동": "연남", "한남동": "한남", "연희동": "연희"}


def normalize(text):
    """같은 검색어가 대소문자/유니코드 조합 차이로 다른 캐시 키가 되지 않게 한다."""
    return " ".join(unicodedata.normalize("NFKC", text).casefold().split())


def match_key(text):
    """화면의 표기는 보존하고 검색/캐시 비교에만 쓰는 형태를 만든다.

    '성수동카페', '성수 카페', '성수카페' → '성수카페'.
    아무 글자의 '동'을 지우지 않고 지원하는 동네 이름만 별칭 처리한다.
    자동완성과 최종 검색이 이 함수를 공유해야 추천을 선택한 뒤 결과가 사라지지 않는다.
    """
    value = normalize(text).replace(" ", "")
    for alias, district in DISTRICT_ALIASES.items():
        value = value.replace(alias, district)
    return value


def search_tokens(text):
    """붙여 쓴 '성수카페'도 ['성수', '카페']로 나눠 띄어쓰기와 결과를 일치시킨다.

    단순 공백 제거만 하면 '성수 카페'는 북카페도 찾지만 '성수카페'는 놓칠 수 있다.
    등록된 동네가 앞/뒤에 있을 때 같은 토큰으로 검색하고, 그 외에는 입력 공백을 쓴다.
    """
    compact = match_key(text)
    districts = sorted({place["district"] for place in PLACES}, key=len, reverse=True)
    for district in districts:
        if len(compact) > len(district):
            if compact.startswith(district):
                return [district, compact[len(district):]]
            if compact.endswith(district):
                return [compact[:-len(district)], district]
    return [match_key(part) for part in normalize(text).split()]


def initials(text):
    # NFKC는 ㄱ 등을 초성 자모(ᄀ)로 바꾼다. 출력도 normalize해 입력과 비교한다.
    return normalize("".join(INITIALS[(ord(c) - 0xAC00) // 588]
                             if 0xAC00 <= ord(c) <= 0xD7A3 else c for c in text))


class SearchService:
    def __init__(self, db_path, client, cache_enabled=True, event_count=100000):
        self.db_path = Path(db_path)
        self.client = client
        self.cache_enabled = cache_enabled
        self.stats = Counter()
        self.stats_lock = threading.Lock()
        # 같은 prefix가 동시에 MISS일 때 DB 집계가 폭증하지 않도록 묶는다.
        # 고정 개수의 lock stripe라 검색어가 늘어도 lock 메모리가 무한히 늘지 않는다.
        self.prefix_locks = [threading.Lock() for _ in range(64)]
        corpus = json.dumps(PLACES, ensure_ascii=False, sort_keys=True).encode()
        # 매칭 규칙 변경 전의 빈 결과/후보를 재사용하지 않도록 버전도 바꾼다.
        self.namespace = "teum:suggest:v3:" + hashlib.sha256(corpus).hexdigest()[:10] + ":"
        self.seed(event_count)

    @contextmanager
    def database(self):
        connection = sqlite3.connect(self.db_path, timeout=5)
        try:
            with connection:
                yield connection
        finally:
            connection.close()

    def count(self, key, amount=1):
        with self.stats_lock:
            self.stats[key] += amount

    def snapshot(self):
        with self.stats_lock:
            result = dict(self.stats)
        total = result.get("hits", 0) + result.get("misses", 0) + result.get("bypasses", 0)
        result["hit_rate"] = round(result.get("hits", 0) / total, 4) if total else 0
        return result

    def seed(self, event_count):
        self.db_path.parent.mkdir(parents=True, exist_ok=True)
        with self.database() as db:
            db.execute("PRAGMA journal_mode=WAL")
            db.execute("CREATE TABLE IF NOT EXISTS meta (version TEXT PRIMARY KEY)")
            if db.execute("SELECT 1 FROM meta WHERE version=?", (self.namespace,)).fetchone():
                return
            db.executescript("""
                CREATE TABLE IF NOT EXISTS phrases (
                    id INTEGER PRIMARY KEY, text TEXT UNIQUE NOT NULL,
                    normalized TEXT NOT NULL, initials TEXT NOT NULL, kind TEXT NOT NULL);
                CREATE TABLE IF NOT EXISTS search_events (
                    id INTEGER PRIMARY KEY, phrase_id INTEGER NOT NULL);
                CREATE INDEX IF NOT EXISTS events_phrase ON search_events(phrase_id);
            """)
            db.execute("DELETE FROM search_events")
            db.execute("DELETE FROM phrases")
            db.execute("DELETE FROM meta")
            phrases = {}
            for place in PLACES:
                phrases[place["name"]] = place["category_label"]
                for tag in place["tags"]:
                    phrases.setdefault(tag, "키워드")
            for phrase in STARTERS:
                phrases[phrase] = "키워드"
            for category in CATEGORIES:
                if category["id"] != "all":
                    phrases[category["label"]] = "종류"
            for text, kind in phrases.items():
                db.execute("INSERT INTO phrases(text,normalized,initials,kind) VALUES(?,?,?,?)",
                           (text, match_key(text), initials(match_key(text)), kind))
            ids = [row[0] for row in db.execute("SELECT id FROM phrases ORDER BY id")]
            rng = random.Random(42)
            # sleep으로 지연을 꾸미지 않는다. 합성 검색 이력에서 실제 GROUP BY를 수행한다.
            db.executemany("INSERT INTO search_events(phrase_id) VALUES(?)",
                           ((rng.choice(ids),) for _ in range(event_count)))
            db.execute("INSERT INTO meta(version) VALUES(?)", (self.namespace,))

    def key(self, query):
        return self.namespace + hashlib.sha256(match_key(query).encode()).hexdigest()

    def _from_cache(self, key):
        raw = self.client.get(key)
        if raw is None:
            return None
        try:
            value = json.loads(raw)
            if (isinstance(value, list) and len(value) <= 8 and all(
                    isinstance(item, dict) and isinstance(item.get("text"), str)
                    and isinstance(item.get("kind"), str) for item in value)):
                return value
        except (ValueError, UnicodeError):
            pass
        return None

    def aggregate(self, query):
        self.count("db_reads")
        # LIKE의 %/_가 와일드카드가 되지 않게 escape한다. SQL은 매개변수로 전달한다.
        escaped = query.replace("\\", "\\\\").replace("%", "\\%").replace("_", "\\_") + "%"
        with self.database() as db:
            rows = db.execute("""
                SELECT p.text, p.kind, COUNT(e.id) AS popularity
                FROM phrases p LEFT JOIN search_events e ON e.phrase_id=p.id
                WHERE p.normalized LIKE ? ESCAPE '\\' OR p.initials LIKE ? ESCAPE '\\'
                GROUP BY p.id ORDER BY (p.normalized=?) DESC, popularity DESC, p.text
                LIMIT 8
            """, (escaped, escaped, query)).fetchall()
        return [{"text": row[0], "kind": row[1]} for row in rows]

    def suggest(self, raw_query):
        normalized = normalize(raw_query)
        if len(normalized) > 80:
            raise ValueError("검색어는 80자 이내로 입력해 주세요.")
        query = match_key(normalized)
        if not query:
            return [{"text": text, "kind": "키워드"} for text in STARTERS], "BYPASS"
        if not self.cache_enabled:
            self.count("bypasses")
            return self.aggregate(query), "BYPASS"
        key = self.key(query)
        try:
            cached = self._from_cache(key)
            if cached is not None:  # 빈 목록도 유효한 캐시다. truthiness로 판단하지 않는다.
                self.count("hits")
                return cached, "HIT"
        except (OSError, MiniRedisError):
            self.count("bypasses")
            return self.aggregate(query), "BYPASS"
        stripe = int(hashlib.sha256(query.encode()).hexdigest()[:8], 16) % len(self.prefix_locks)
        with self.prefix_locks[stripe]:
            values = None
            try:
                # 기다리는 동안 먼저 들어온 요청이 채웠을 수 있으므로 double-check.
                cached = self._from_cache(key)
                if cached is not None:
                    self.count("hits")
                    return cached, "HIT"
                values = self.aggregate(query)
                self.client.set(key, json.dumps(values, ensure_ascii=False).encode(), ttl_ms=30000)
            except (OSError, MiniRedisError):
                self.count("bypasses")
                # SET 실패라면 계산한 결과를 재사용해 DB를 두 번 읽지 않는다.
                return (values if values is not None else self.aggregate(query)), "BYPASS"
        self.count("misses")
        return values, "MISS"

    def search(self, query="", category="all", district="all", saved=None, sort="recommended"):
        query = normalize(query)
        if len(query) > 80:
            raise ValueError("검색어는 80자 이내로 입력해 주세요.")
        if category not in {item["id"] for item in CATEGORIES}:
            raise ValueError("알 수 없는 카테고리입니다.")
        compact_query = match_key(query)
        tokens = search_tokens(query)
        results = []
        for place in PLACES:
            if category != "all" and place["category"] != category:
                continue
            if district != "all" and place["district"] != district:
                continue
            if saved is not None and place["id"] not in saved:
                continue
            haystack = match_key(" ".join([place["name"], place["district"], place["category_label"], *place["tags"]]))
            initial_haystack = initials(haystack)
            if compact_query and compact_query not in haystack and compact_query not in initial_haystack:
                # 띄어쓴 여러 단어는 순서를 바꿔도 검색한다. '카페 성수동'도 처리한다.
                if not all(part in haystack or part in initial_haystack for part in tokens):
                    continue
            results.append(place)
        if sort == "name":
            results.sort(key=lambda place: place["name"])
        return results


class SearchHttpServer(ThreadingHTTPServer):
    request_queue_size = 64
    daemon_threads = True


def handler_for(service):
    class Handler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"
        def send_json(self, data, status=200, source=None):
            self.close_connection = True
            body = json.dumps(data, ensure_ascii=False).encode()
            self.send_response(status)
            self.send_header("Content-Type", "application/json; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.send_header("Connection", "close")
            if source:
                self.send_header("X-Cache", source)
            self.end_headers()
            try:
                self.wfile.write(body)
                self.wfile.flush()
            except (BrokenPipeError, ConnectionResetError):
                pass  # 입력이 바뀌어 브라우저가 이전 자동완성 요청을 취소할 수 있다.

        def do_GET(self):
            parsed = urlsplit(self.path)
            if len(self.path) > 4096:
                return self.send_json({"error": "요청이 너무 깁니다."}, 414)
            params = parse_qs(parsed.query, keep_blank_values=True)
            get = lambda key, default="": params.get(key, [default])[0]
            try:
                if parsed.path == "/api/suggest":
                    start = time.perf_counter()
                    values, source = service.suggest(get("q"))
                    service.count("suggest_requests")
                    service.count("suggest_microseconds", round((time.perf_counter() - start) * 1000000))
                    return self.send_json({"suggestions": values}, source=source)
                if parsed.path == "/api/search":
                    saved = None
                    if "saved" in params:
                        values = get("saved").split(",")
                        saved = {int(value) for value in values if value.isascii() and value.isdigit() and len(value) < 8}
                    places = service.search(get("q"), get("category", "all"), get("district", "all"), saved, get("sort"))
                    return self.send_json({"places": places, "total": len(places)})
                if parsed.path == "/api/config":
                    return self.send_json({"categories": CATEGORIES, "starters": STARTERS,
                                           "total": len(PLACES), "districts": sorted({p["district"] for p in PLACES})})
                if parsed.path == "/api/stats":
                    return self.send_json(service.snapshot())
            except ValueError as exc:
                return self.send_json({"error": str(exc)}, 400)
            except sqlite3.Error:
                return self.send_json({"error": "잠시 후 다시 검색해 주세요."}, 503)
            if parsed.path.startswith("/api/"):
                return self.send_json({"error": "페이지를 찾을 수 없습니다."}, 404)
            self.serve_file(parsed.path)

        def serve_file(self, path):
            # URL 경로를 정규화하고 웹 폴더 밖 파일은 절대 제공하지 않는다.
            target = (WEB_ROOT / unquote(path).lstrip("/")).resolve()
            if path == "/":
                target = WEB_ROOT / "index.html"
            if not target.is_relative_to(WEB_ROOT.resolve()) or not target.is_file():
                self.send_error(HTTPStatus.NOT_FOUND)
                return
            types = {".html": "text/html; charset=utf-8", ".css": "text/css; charset=utf-8",
                     ".js": "text/javascript; charset=utf-8", ".jpg": "image/jpeg", ".svg": "image/svg+xml"}
            body = target.read_bytes()
            self.close_connection = True
            self.send_response(200)
            self.send_header("Content-Type", types.get(target.suffix, "application/octet-stream"))
            # 이 환경의 HTTP 경로에서 큰 고정 길이 응답의 끝부분이 전달되지 않는
            # 현상을 재현했다. HTTP/1.1의 표준 chunked framing으로 경계를 명확히
            # 보낸다. 파일 내용을 바꾸거나 패딩하지 않으며, 테스트는 원본과 비교한다.
            chunked = self.request_version == "HTTP/1.1"
            if chunked:
                self.send_header("Transfer-Encoding", "chunked")
            else:
                self.send_header("Content-Length", str(len(body)))
            self.send_header("X-Content-Type-Options", "nosniff")
            self.send_header("Cache-Control", "no-cache")
            self.send_header("Connection", "close")
            self.end_headers()
            try:
                if chunked:
                    for offset in range(0, len(body), 16384):
                        chunk = body[offset:offset + 16384]
                        self.wfile.write(f"{len(chunk):x}\r\n".encode() + chunk + b"\r\n")
                    self.wfile.write(b"0\r\n\r\n")
                else:
                    self.wfile.write(body)
                self.wfile.flush()
            except (BrokenPipeError, ConnectionResetError):
                pass

        def log_message(self, format_string, *values):
            pass
    return Handler


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, default=8090)
    parser.add_argument("--redis-port", type=int, default=6379)
    parser.add_argument("--cache", choices=("on", "off"), default="on")
    parser.add_argument("--db", type=Path, default=ROOT / "build" / "teum.db")
    args = parser.parse_args()
    service = SearchService(args.db, MiniRedis(port=args.redis_port, timeout=0.3), args.cache == "on")
    server = SearchHttpServer(("127.0.0.1", args.port), handler_for(service))
    print(f"틈 http://127.0.0.1:{args.port} (autocomplete service)", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
