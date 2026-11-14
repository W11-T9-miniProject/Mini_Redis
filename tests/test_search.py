"""자동완성의 초성/정규화/SQL 경계와 캐시 의미를 네트워크 없이 검증한다."""

from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import sys
import tempfile
import threading
import unittest
import json
from urllib.error import HTTPError
from urllib.parse import urlencode
from urllib.request import urlopen

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "examples"))
from search_app import SearchService, SearchHttpServer, handler_for, initials, normalize


class Cache:
    def __init__(self):
        self.values = {}
        self.lock = threading.Lock()
        self.unavailable = False
        self.set_fails = False

    def get(self, key):
        if self.unavailable:
            raise OSError("unavailable")
        with self.lock:
            return self.values.get(key)

    def set(self, key, value, ttl_ms):
        if self.set_fails:
            raise OSError("set failed")
        if ttl_ms != 30000:
            raise AssertionError("unexpected TTL")
        with self.lock:
            self.values[key] = value


class SearchTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.cache = Cache()
        self.service = SearchService(Path(self.temp.name) / "test.db", self.cache, event_count=3000)

    def test_miss_then_hit_and_empty_result_cached(self):
        first, source = self.service.suggest("성수")
        self.assertEqual(source, "MISS")
        self.assertTrue(first)
        self.assertEqual(self.service.suggest("성수"), (first, "HIT"))
        self.assertEqual(self.service.stats["db_reads"], 1)
        self.assertEqual(self.service.suggest("없는검색어xyz"), ([], "MISS"))
        self.assertEqual(self.service.suggest("없는검색어xyz"), ([], "HIT"))

    def test_initials_and_unicode_normalization(self):
        self.assertEqual(initials("성수"), normalize("ㅅㅅ"))
        values, _ = self.service.suggest("ㅅㅅ")
        self.assertTrue(any("성수" in value["text"] for value in values))
        self.assertEqual(normalize("  성수   카페  "), "성수 카페")
        before = self.service.suggest("성수 카페")
        after = self.service.suggest("  성수   카페 ")
        self.assertEqual(before[0], after[0])
        self.assertEqual(after[1], "HIT")

    def test_sql_wildcards_and_query_limit(self):
        for query in ("%", "_", "' OR 1=1 --", "\\"):
            self.assertEqual(self.service.suggest(query)[0], [])
        with self.assertRaises(ValueError):
            self.service.suggest("a" * 81)
        self.assertTrue(self.service.suggest("")[0])
        self.assertEqual(self.service.stats["db_reads"], 4)

    def test_fallback_and_set_failure_do_not_read_db_twice(self):
        self.cache.unavailable = True
        self.assertEqual(self.service.suggest("서촌")[1], "BYPASS")
        self.assertEqual(self.service.stats["db_reads"], 1)
        self.cache.unavailable = False
        self.cache.set_fails = True
        self.assertEqual(self.service.suggest("연남")[1], "BYPASS")
        self.assertEqual(self.service.stats["db_reads"], 2)

    def test_corrupt_cached_value_is_rebuilt(self):
        key = self.service.key(normalize("카페"))
        for value in (b"not json", b"null", b"[1,2]", b'[{"text":null}]'):
            self.cache.values[key] = value
            suggestions, source = self.service.suggest("카페")
            self.assertEqual(source, "MISS")
            self.assertTrue(suggestions)

    def test_same_prefix_concurrency_coalesces_db_work(self):
        barrier = threading.Barrier(8)

        def worker(_):
            barrier.wait()
            return self.service.suggest("성수")

        with ThreadPoolExecutor(max_workers=8) as pool:
            results = list(pool.map(worker, range(8)))
        self.assertTrue(all(result[0] == results[0][0] for result in results))
        self.assertEqual(self.service.stats["db_reads"], 1)
        self.assertEqual(sum(result[1] == "MISS" for result in results), 1)

    def test_search_filters_and_saved_view(self):
        self.assertTrue(self.service.search("성수 카페"))
        values = self.service.search("성수", category="cafe")
        self.assertTrue(values)
        self.assertTrue(all(value["district"] == "성수" and value["category"] == "cafe" for value in values))
        self.assertEqual(self.service.search(saved=set()), [])
        self.assertEqual([value["id"] for value in self.service.search(saved={1,2})], [1,2])
        self.assertTrue(all(value["category"] == "books" for value in self.service.search(category="books")))
        self.assertTrue(self.service.search("ㅅㅅ"))
        with self.assertRaises(ValueError):
            self.service.search(category="invalid")


class SearchHttpTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.service = SearchService(Path(cls.temp.name) / "http.db", Cache(), event_count=1000)
        cls.server = SearchHttpServer(("127.0.0.1", 0), handler_for(cls.service))
        cls.thread = threading.Thread(target=cls.server.serve_forever, daemon=True)
        cls.thread.start()
        cls.base = "http://127.0.0.1:" + str(cls.server.server_port)

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()
        cls.server.server_close()
        cls.thread.join(timeout=2)
        cls.temp.cleanup()

    def get(self, path):
        try:
            response = urlopen(self.base + path, timeout=3)
        except HTTPError as error:
            response = error
        with response:
            return response.status, response.headers, response.read()

    def test_html_and_local_assets(self):
        status, headers, body = self.get("/")
        self.assertEqual(status, 200)
        self.assertIn("text/html", headers["Content-Type"])
        self.assertIn("익숙한 도시", body.decode())
        self.assertNotIn("Redis", body.decode())
        for path in ("/app.js", "/styles.css", "/assets/favicon.svg", "/assets/cafe.jpg",
                     "/assets/forest.jpg", "/assets/books.jpg", "/assets/gallery.jpg",
                     "/assets/interior.jpg", "/assets/architecture.jpg"):
            with self.subTest(path=path):
                status, _, body = self.get(path)
                self.assertEqual(status, 200)
                source = Path(__file__).resolve().parents[1] / "web" / "teum" / path.lstrip("/")
                self.assertEqual(body, source.read_bytes())

    def test_autocomplete_over_http_and_limits(self):
        path = "/api/suggest?" + urlencode({"q": "성수"})
        first = self.get(path)
        second = self.get(path)
        self.assertEqual(first[0], 200)
        self.assertTrue(json.loads(first[2])["suggestions"])
        self.assertEqual(second[1]["X-Cache"], "HIT")
        self.assertEqual(first[2], second[2])
        self.assertEqual(self.get("/api/suggest?" + urlencode({"q":"가" * 81}))[0], 400)
        self.assertEqual(self.get("/api/suggest?q=%25")[0], 200)

    def test_filter_and_saved_routes(self):
        config = json.loads(self.get("/api/config")[2])
        self.assertEqual(config["total"], 30)
        values = json.loads(self.get("/api/search?category=books")[2])["places"]
        self.assertTrue(values)
        self.assertTrue(all(value["category"] == "books" for value in values))
        self.assertEqual(json.loads(self.get("/api/search?saved=")[2])["places"], [])
        self.assertEqual(self.get("/api/search?category=invalid")[0], 400)
        stats = json.loads(self.get("/api/stats")[2])
        self.assertIn("hit_rate", stats)

    def test_missing_and_traversal_paths(self):
        for path in ("/api/does-not-exist", "/missing.css", "/%2e%2e/README.md",
                     "/assets/%2e%2e/%2e%2e/README.md"):
            self.assertEqual(self.get(path)[0], 404)


if __name__ == "__main__":
    unittest.main()
