"""실제 C 저장소와 검색 HTTP 서버를 연결한 과제 중점 항목 검증.

시험 전용 SQLite와 UUID 캐시 namespace를 써서 실행 중 웹의 키를 건드리지 않는다.
DEL은 외부 클라이언트가 호출한다. 일반 사용자 화면에 관리용 버튼을 추가하지 않는다.
"""

from concurrent.futures import ThreadPoolExecutor
import json
from pathlib import Path
import sys
import tempfile
import threading
import time
import unittest
from urllib.parse import urlencode
from urllib.request import Request, urlopen
import uuid

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "examples"))
from mini_client import MiniRedis
from search_app import SearchService, SearchHttpServer, handler_for, match_key, initials
from test_server import HOST, PORT


class SearchCacheIntegrationTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.client = MiniRedis(HOST, PORT, timeout=2)
        self.assertEqual(self.client.execute("PING"), b"PONG")
        self.service = SearchService(Path(self.temp.name) / "search.db", self.client, event_count=1000)
        self.service.namespace += "verify:" + uuid.uuid4().hex + ":"
        self.query = "성수"
        self.key = self.service.key(self.query)
        self.observed_keys = {self.key}
        self.server = SearchHttpServer(("127.0.0.1", 0), handler_for(self.service))
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.base = "http://127.0.0.1:" + str(self.server.server_port)
        self.url = self.base + "/api/suggest?" + urlencode({"q": self.query})

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=2)
        try:
            for key in self.observed_keys:
                self.client.delete(key)
        finally:
            self.temp.cleanup()

    def request(self, query=None):
        query = query or self.query
        self.observed_keys.add(self.service.key(query))
        with urlopen(self.base + "/api/suggest?" + urlencode({"q": query}), timeout=3) as response:
            return json.loads(response.read())["suggestions"], response.headers["X-Cache"]

    def test_admin_http_crud_issues_real_del_automatically(self):
        query = "관리검증"
        self.assertEqual(self.request(query), ([], "MISS"))
        old_key = self.service.key(query)
        self.assertIsNotNone(self.client.get(old_key))
        with urlopen(self.base + "/api/admin/session", timeout=3) as response:
            token = json.loads(response.read())["token"]

        def change(method, suffix, data):
            request = Request(self.base + "/api/admin/phrases" + suffix, method=method,
                              data=json.dumps(data).encode(),
                              headers={"Content-Type": "application/json", "X-Admin-Token": token})
            with urlopen(request, timeout=5) as response:
                return json.loads(response.read())

        created = change("POST", "", {"text": "관리검증 책방", "kind": "서점"})
        self.assertGreaterEqual(created["invalidation"]["deleted"], 1)
        self.assertIsNone(self.client.get(old_key))
        self.assertIn("관리검증 책방", [row["text"] for row in self.request(query)[0]])
        old_key = self.service.key(query)
        phrase_id = str(created["phrase"]["id"])
        change("PUT", "/" + phrase_id, {"text": "관리검증 작은책방", "kind": "서점"})
        self.assertIsNone(self.client.get(old_key))
        self.assertIn("관리검증 작은책방", [row["text"] for row in self.request(query)[0]])
        old_key = self.service.key(query)
        change("DELETE", "/" + phrase_id, {})
        self.assertIsNone(self.client.get(old_key))
        self.assertEqual(self.request(query), ([], "MISS"))

    def test_expired_result_is_not_served_and_is_rebuilt(self):
        original, source = self.request()
        self.assertEqual(source, "MISS")
        self.assertEqual(self.request(), (original, "HIT"))
        # 웹의 기본 TTL은 30초. 테스트에서는 EXPIRE로 기한을 1초로 줄여 대기를 제한.
        self.assertEqual(self.client.execute("EXPIRE", self.key, 1), 1)
        deadline = time.monotonic() + 3
        while self.client.get(self.key) is not None:
            if time.monotonic() >= deadline:
                self.fail("cached suggestion did not expire")
            time.sleep(.02)
        self.assertEqual(self.request(), (original, "MISS"))
        self.assertEqual(self.service.stats["db_reads"], 2)

    def test_external_del_exposes_updated_source(self):
        original, source = self.request()
        self.assertEqual(source, "MISS")
        phrase = "성수 새카페"
        with self.service.database() as db:
            cursor = db.execute("INSERT INTO phrases(text,normalized,initials,kind) VALUES(?,?,?,?)",
                                (phrase, match_key(phrase), initials(match_key(phrase)), "키워드"))
            db.executemany("INSERT INTO search_events(phrase_id) VALUES(?)", [(cursor.lastrowid,)] * 500)
        self.assertEqual(self.request(), (original, "HIT"), "무효화 전에는 기존 후보를 재사용")
        self.assertEqual(self.client.delete(self.key), 1)
        self.assertIsNone(self.client.get(self.key))
        updated, source = self.request()
        self.assertEqual(source, "MISS")
        self.assertIn(phrase, [item["text"] for item in updated])
        self.assertEqual(self.service.stats["db_reads"], 2)

    def test_concurrent_http_requests_share_one_db_calculation(self):
        barrier = threading.Barrier(8)

        def request(_):
            barrier.wait()
            return self.request()

        with ThreadPoolExecutor(max_workers=8) as pool:
            results = list(pool.map(request, range(8)))
        self.assertTrue(all(result[0] == results[0][0] for result in results))
        self.assertEqual(sum(result[1] == "MISS" for result in results), 1)
        self.assertEqual(sum(result[1] == "HIT" for result in results), 7)
        self.assertEqual(self.service.stats["db_reads"], 1)


if __name__ == "__main__":
    unittest.main()
