"""틈: 로컬 정적 웹 + SQLite 자동완성 + C Mini Redis를 연결하는 HTTP 서버.

실행: python examples/search_app.py --port 8090
흐름: 브라우저 → /api/suggest → Mini Redis GET → MISS면 SQLite 집계 → SET PX.
응답의 X-Cache와 /api/stats는 개발자 측정용이며 웹 UI에 노출하지 않는다.
"""

import argparse
from collections import Counter
from contextlib import contextmanager, ExitStack
import hashlib
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
import random
import secrets
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
        self.catalog_write_lock = threading.Lock()
        self.pending_delete_lock = threading.Lock()
        self.admin_token = secrets.token_urlsafe(32)
        self.next_delete_retry = 0
        corpus = json.dumps(PLACES, ensure_ascii=False, sort_keys=True).encode()
        # 매칭 규칙 변경 전의 빈 결과/후보를 재사용하지 않도록 버전도 바꾼다.
        self.namespace = "teum:suggest:v3:" + hashlib.sha256(corpus).hexdigest()[:10] + ":"
        self.seed(event_count)
        with self.database() as db:
            self.prefix_revisions = dict(db.execute("SELECT query,revision FROM cache_revisions"))
            self.pending_delete_count = db.execute("SELECT COUNT(*) FROM pending_cache_deletes").fetchone()[0]
            self.initial_suggestions = self.starting_suggestions(db)

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
            # 관리 편집은 seed 버전과 별도로 저장된다. 같은 버전으로 재시작해도
            # 편집한 추천어/무효화 번호/DEL 재시도 목록을 초기화하지 않는다.
            db.execute("CREATE TABLE IF NOT EXISTS cache_revisions (query TEXT PRIMARY KEY, revision INTEGER NOT NULL)")
            db.execute("CREATE TABLE IF NOT EXISTS pending_cache_deletes (cache_key TEXT PRIMARY KEY)")
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

    def key(self, query, revision=None):
        query = match_key(query)
        if revision is None:
            revision = self.prefix_revisions.get(query, 0)
        return self.namespace + f"r{revision}:" + hashlib.sha256(query.encode()).hexdigest()

    def stripe(self, query):
        return int(hashlib.sha256(query.encode()).hexdigest()[:8], 16) % len(self.prefix_locks)

    @staticmethod
    def starting_suggestions(db):
        # 빈 검색창도 관리자가 삭제/이름 변경한 추천어를 되살려 보여주지 않는다.
        rows = dict(db.execute("SELECT text,kind FROM phrases WHERE text IN (" + ",".join("?" for _ in STARTERS) + ")", STARTERS))
        return [{"text": text, "kind": rows[text]} for text in STARTERS if text in rows]

    @staticmethod
    def affected_prefixes(*phrases):
        # 예: '성수 카페' 변경은 '성', '성수', '성수카'와 초성 'ㅅ', 'ㅅㅅ'도 바꾼다.
        # 이름을 바꾸면 이전 이름과 새 이름의 접두어를 모두 무효화해야 한다.
        values = set()
        for phrase in phrases:
            if phrase:
                compact = match_key(phrase)
                for text in (compact, initials(compact)):
                    values.update(text[:end] for end in range(1, len(text) + 1))
        return values

    def retry_pending_deletes(self, force=False):
        # DEL 실패도 DB 변경은 이미 커밋된 상태다. 이전 캐시를 쓰지 않도록 버전을
        # 바꾸고, 삭제할 실제 C 키는 SQLite 큐에 남겨 다음 요청에서 재시도한다.
        if not self.pending_delete_count and not force:
            return {"attempted": 0, "deleted": 0, "pending": 0}
        if not force and time.monotonic() < self.next_delete_retry:
            return {"attempted": 0, "deleted": 0, "pending": self.pending_delete_count}
        with self.pending_delete_lock:
            limit = 400 if force else 8
            with self.database() as db:
                keys = [row[0] for row in db.execute("SELECT cache_key FROM pending_cache_deletes LIMIT ?", (limit,))]
            attempted = deleted = 0
            for key in keys:
                attempted += 1
                self.count("invalidation_attempts")
                try:
                    removed = self.client.delete(key)
                except (OSError, MiniRedisError):
                    self.count("invalidation_errors")
                    break
                deleted += int(removed)
                self.count("invalidated_entries", int(removed))
                with self.database() as db:
                    db.execute("DELETE FROM pending_cache_deletes WHERE cache_key=?", (key,))
            with self.database() as db:
                self.pending_delete_count = db.execute("SELECT COUNT(*) FROM pending_cache_deletes").fetchone()[0]
            self.next_delete_retry = time.monotonic() + 1
            return {"attempted": attempted, "deleted": deleted, "pending": self.pending_delete_count}

    def list_phrases(self, query="", page=1):
        query = match_key(query)
        if len(query) > 80 or page < 1 or page > 100000:
            raise ValueError("검색어나 페이지 번호를 확인해 주세요.")
        escaped = "%" + query.replace("\\", "\\\\").replace("%", "\\%").replace("_", "\\_") + "%"
        with self.database() as db:
            total = db.execute("SELECT COUNT(*) FROM phrases WHERE normalized LIKE ? ESCAPE '\\'", (escaped,)).fetchone()[0]
            rows = db.execute("""SELECT id,text,kind FROM phrases
                WHERE normalized LIKE ? ESCAPE '\\' ORDER BY normalized,id LIMIT 50 OFFSET ?""",
                (escaped, (page - 1) * 50)).fetchall()
        return {"phrases": [{"id": row[0], "text": row[1], "kind": row[2]} for row in rows],
                "total": total, "page": page, "page_size": 50, "pending": self.pending_delete_count}

    def edit_phrase(self, action, phrase_id=None, text=None, kind=None):
        if action not in {"create", "update", "delete"}:
            raise ValueError("알 수 없는 작업입니다.")
        if action != "create" and (not isinstance(phrase_id, int) or isinstance(phrase_id, bool) or phrase_id <= 0):
            raise ValueError("검색어 번호를 확인해 주세요.")
        if action != "delete":
            if not isinstance(text, str) or not isinstance(kind, str):
                raise ValueError("검색어와 분류를 입력해 주세요.")
            if not 1 <= len(text.strip()) <= 80 or not 1 <= len(kind.strip()) <= 20:
                raise ValueError("검색어는 1~80자, 분류는 1~20자로 입력해 주세요.")
            if any(unicodedata.category(char).startswith("C") for char in text + kind):
                raise ValueError("제어 문자는 사용할 수 없습니다.")
            text, kind = " ".join(text.split()), " ".join(kind.split())
        with self.catalog_write_lock:
            with self.database() as db:
                old = None if action == "create" else db.execute("SELECT text,normalized FROM phrases WHERE id=?", (phrase_id,)).fetchone()
                if action != "create" and old is None:
                    raise LookupError("해당 검색어를 찾을 수 없습니다.")
                if action != "delete" and db.execute("SELECT 1 FROM phrases WHERE text=? AND id!=?", (text, phrase_id or -1)).fetchone():
                    raise ValueError("같은 검색어가 이미 있습니다.")
                if action != "delete" and (old is None or match_key(text) != old[1]):
                    if db.execute("SELECT 1 FROM phrases WHERE normalized=?", (match_key(text),)).fetchone():
                        raise ValueError("같은 의미의 검색어가 이미 있습니다.")
            affected = self.affected_prefixes(old[0] if old else None, text if action != "delete" else None)
            # 모든 요청을 잠그지 않고 영향을 받는 prefix의 stripe만 고정 순서로 잠근다.
            # DB 집계→SET 도중 편집→DEL이 끼어들어 오래된 결과가 되살아나는 것을 막는다.
            with ExitStack() as locks:
                for index in sorted({self.stripe(prefix) for prefix in affected}):
                    locks.enter_context(self.prefix_locks[index])
                old_keys = [self.key(prefix) for prefix in affected]
                revisions = {}
                with self.database() as db:
                    if action == "create":
                        phrase_id = db.execute("INSERT INTO phrases(text,normalized,initials,kind) VALUES(?,?,?,?)",
                            (text, match_key(text), initials(match_key(text)), kind)).lastrowid
                    elif action == "update":
                        db.execute("UPDATE phrases SET text=?,normalized=?,initials=?,kind=? WHERE id=?",
                            (text, match_key(text), initials(match_key(text)), kind, phrase_id))
                    else:
                        db.execute("DELETE FROM search_events WHERE phrase_id=?", (phrase_id,))
                        db.execute("DELETE FROM phrases WHERE id=?", (phrase_id,))
                    for prefix in affected:
                        db.execute("""INSERT INTO cache_revisions(query,revision) VALUES(?,1)
                            ON CONFLICT(query) DO UPDATE SET revision=revision+1""", (prefix,))
                        revisions[prefix] = db.execute("SELECT revision FROM cache_revisions WHERE query=?", (prefix,)).fetchone()[0]
                    db.executemany("INSERT OR IGNORE INTO pending_cache_deletes(cache_key) VALUES(?)", ((key,) for key in old_keys))
                    initial_suggestions = self.starting_suggestions(db)
                # 위 with를 벗어나면 커밋 완료. 새 요청은 새 버전 키만 사용한다.
                self.prefix_revisions.update(revisions)
                self.initial_suggestions = initial_suggestions
                self.pending_delete_count += len(old_keys)
                invalidation = self.retry_pending_deletes(force=True)
            self.count("admin_changes")
            return {"phrase": None if action == "delete" else {"id": phrase_id, "text": text, "kind": kind},
                    "invalidation": {"affected": len(affected), **invalidation}}

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
        self.retry_pending_deletes()
        normalized = normalize(raw_query)
        if len(normalized) > 80:
            raise ValueError("검색어는 80자 이내로 입력해 주세요.")
        query = match_key(normalized)
        if not query:
            return list(self.initial_suggestions), "BYPASS"
        if not self.cache_enabled:
            self.count("bypasses")
            return self.aggregate(query), "BYPASS"
        revision = self.prefix_revisions.get(query, 0)
        key = self.key(query, revision)
        try:
            cached = self._from_cache(key)
            if cached is not None and revision == self.prefix_revisions.get(query, 0):
                self.count("hits")
                return cached, "HIT"
        except (OSError, MiniRedisError):
            self.count("bypasses")
            return self.aggregate(query), "BYPASS"
        stripe = self.stripe(query)
        with self.prefix_locks[stripe]:
            key = self.key(query)
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

        def admin_allowed(self, write=False):
            # 로컬 데모용 관리 기능이다. 외부 웹페이지의 폼/스크립트나 DNS rebinding으로
            # localhost의 추천어가 바뀌지 않게 Host/Origin과 세션 토큰을 함께 확인한다.
            host = self.headers.get("Host", "").lower()
            allowed = {f"127.0.0.1:{self.server.server_port}", f"localhost:{self.server.server_port}"}
            origin = self.headers.get("Origin")
            if host not in allowed or (origin and origin != f"http://{host}") or self.headers.get("Sec-Fetch-Site") == "cross-site":
                self.send_json({"error": "이 관리 기능은 같은 주소에서만 사용할 수 있습니다."}, 403)
                return False
            if write:
                token = self.headers.get("X-Admin-Token", "")
                if not token.isascii() or not secrets.compare_digest(token, service.admin_token):
                    self.send_json({"error": "관리 화면을 새로고침한 뒤 다시 시도해 주세요."}, 403)
                    return False
                if self.headers.get_content_type() != "application/json":
                    self.send_json({"error": "JSON 형식의 요청이 필요합니다."}, 415)
                    return False
            return True

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
                if parsed.path.startswith("/api/admin/"):
                    if not self.admin_allowed():
                        return
                    if parsed.path == "/api/admin/session":
                        return self.send_json({"token": service.admin_token})
                    if parsed.path == "/api/admin/phrases":
                        return self.send_json(service.list_phrases(get("q"), int(get("page", "1"))))
                    return self.send_json({"error": "관리 경로를 찾을 수 없습니다."}, 404)
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
                    return self.send_json({"categories": CATEGORIES, "starters": [item["text"] for item in service.initial_suggestions],
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

        def do_POST(self):
            self.change_phrase("create")

        def do_PUT(self):
            self.change_phrase("update")

        def do_DELETE(self):
            self.change_phrase("delete")

        def change_phrase(self, action):
            parsed = urlsplit(self.path)
            parts = parsed.path.strip("/").split("/")
            if parts[:3] != ["api", "admin", "phrases"] or len(parts) != (3 if action == "create" else 4):
                return self.send_json({"error": "관리 경로를 찾을 수 없습니다."}, 404)
            try:
                self.connection.settimeout(5)
                length = int(self.headers.get("Content-Length", "0"))
                if length < 0 or length > 4096:
                    return self.send_json({"error": "요청 크기를 확인해 주세요."}, 413)
                # 작은 본문을 먼저 받아 두고 권한을 검사한다. 읽지 않은 POST 본문을
                # 남긴 채 연결을 닫으면 Windows에서 RST로 오류 응답까지 사라질 수 있다.
                raw = self.rfile.read(length) if length else b""
                if not self.admin_allowed(write=True):
                    return
                phrase_id = None
                if action != "create":
                    if not parts[3].isascii() or not parts[3].isdigit() or len(parts[3]) > 9:
                        return self.send_json({"error": "검색어 번호를 확인해 주세요."}, 400)
                    phrase_id = int(parts[3])
                data = json.loads(raw) if raw else {}
                if not isinstance(data, dict):
                    raise ValueError("입력 형식을 확인해 주세요.")
                result = service.edit_phrase(action, phrase_id, data.get("text"), data.get("kind"))
                return self.send_json(result, 201 if action == "create" else 200)
            except LookupError as exc:
                return self.send_json({"error": str(exc)}, 404)
            except (ValueError, UnicodeError) as exc:
                return self.send_json({"error": str(exc)}, 400)
            except TimeoutError:
                return self.send_json({"error": "요청 본문을 받는 시간이 초과됐습니다."}, 408)
            except sqlite3.Error:
                return self.send_json({"error": "저장하지 못했습니다. 잠시 후 다시 시도해 주세요."}, 503)

        def serve_file(self, path):
            # URL 경로를 정규화하고 웹 폴더 밖 파일은 절대 제공하지 않는다.
            target = (WEB_ROOT / unquote(path).lstrip("/")).resolve()
            if path == "/":
                target = WEB_ROOT / "index.html"
            if path in {"/admin", "/admin/"}:
                if not self.admin_allowed():
                    return
                target = WEB_ROOT / "admin.html"
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
