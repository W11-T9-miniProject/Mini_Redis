"""추천어 CRUD → 자동 DEL, 변경과 캐시 채우기의 경쟁, 실패/재시작을 검증한다."""

from concurrent.futures import ThreadPoolExecutor
import json
from pathlib import Path
import sys
import tempfile
import threading
import unittest
from urllib.error import HTTPError
from urllib.parse import urlencode
from urllib.request import Request, urlopen

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "examples"))
from search_app import SearchService, SearchHttpServer, handler_for
from test_search import Cache


class AdminTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.cache = Cache()
        self.service = SearchService(Path(self.temp.name) / "admin.db", self.cache, event_count=1000)

    def test_create_invalidates_cached_empty_prefix_and_initials(self):
        for query in ("틈새", "ㅌㅅ"):
            self.assertEqual(self.service.suggest(query), ([], "MISS"))
        keys = [self.service.key(query) for query in ("틈새", "ㅌㅅ")]
        result = self.service.edit_phrase("create", text="틈새 책방", kind="서점")
        self.assertEqual(result["invalidation"]["pending"], 0)
        self.assertEqual(result["invalidation"]["deleted"], 2)
        self.assertTrue(all(self.cache.get(key) is None for key in keys))
        for query in ("틈새", "ㅌㅅ"):
            values, source = self.service.suggest(query)
            self.assertEqual(source, "MISS")
            self.assertIn("틈새 책방", [value["text"] for value in values])

    def test_rename_clears_old_new_and_initial_prefixes_but_keeps_unrelated_cache(self):
        phrase = self.service.edit_phrase("create", text="틈새 책방", kind="서점")["phrase"]
        for query in ("틈새", "ㅌㅅ", "새로", "ㅅㄹ", "연남"):
            self.service.suggest(query)
        old_keys = [self.service.key(query) for query in ("틈새", "ㅌㅅ", "새로", "ㅅㄹ")]
        unrelated = self.service.key("연남")
        result = self.service.edit_phrase("update", phrase["id"], "새로운 책방", "서점")
        self.assertEqual(result["invalidation"]["deleted"], 4)
        self.assertTrue(all(self.cache.get(key) is None for key in old_keys))
        self.assertIsNotNone(self.cache.get(unrelated))
        self.assertEqual(self.service.suggest("틈새"), ([], "MISS"))
        self.assertIn("새로운 책방", [value["text"] for value in self.service.suggest("새로")[0]])
        self.assertEqual(self.service.suggest("연남")[1], "HIT")

    def test_delete_removes_events_phrase_and_cached_value(self):
        phrase = self.service.edit_phrase("create", text="틈새 책방", kind="서점")["phrase"]
        self.service.suggest("틈새")
        key = self.service.key("틈새")
        result = self.service.edit_phrase("delete", phrase["id"])
        self.assertIsNone(result["phrase"])
        self.assertIsNone(self.cache.get(key))
        self.assertEqual(self.service.suggest("틈새"), ([], "MISS"))
        with self.service.database() as db:
            self.assertEqual(db.execute("SELECT COUNT(*) FROM phrases WHERE id=?", (phrase["id"],)).fetchone()[0], 0)

    def test_deleted_starter_is_not_shown_for_empty_input(self):
        self.assertIn("성수 카페", [row["text"] for row in self.service.suggest("")[0]])
        phrase = next(row for row in self.service.list_phrases("성수카페")["phrases"] if row["text"] == "성수 카페")
        self.service.edit_phrase("delete", phrase["id"])
        self.assertNotIn("성수 카페", [row["text"] for row in self.service.suggest("")[0]])

    def test_failed_del_is_fenced_and_retried_after_restart(self):
        self.service.suggest("틈새")
        old_key = self.service.key("틈새")
        self.cache.delete_fails = True
        result = self.service.edit_phrase("create", text="틈새 책방", kind="서점")
        self.assertGreater(result["invalidation"]["pending"], 0)
        self.assertIsNotNone(self.cache.get(old_key), "실패한 DEL의 값은 실제로 남겨 재현")
        values, source = self.service.suggest("틈새")
        self.assertEqual(source, "MISS")
        self.assertIn("틈새 책방", [value["text"] for value in values])
        rebuilt = SearchService(self.service.db_path, self.cache, event_count=1000)
        self.assertEqual(rebuilt.key("틈새"), self.service.key("틈새"))
        self.assertGreater(rebuilt.pending_delete_count, 0)
        self.cache.delete_fails = False
        self.assertEqual(rebuilt.retry_pending_deletes(force=True)["pending"], 0)
        self.assertIsNone(self.cache.get(old_key))
        self.assertTrue(rebuilt.list_phrases("틈새")["phrases"])

    def test_edit_waits_for_inflight_fill_before_del(self):
        original = self.service.aggregate
        has_read, release = threading.Event(), threading.Event()
        old_key = self.service.key("틈새")

        def slow(query):
            result = original(query)
            has_read.set()
            if not release.wait(3):
                raise RuntimeError("test fill not released")
            return result

        self.service.aggregate = slow
        with ThreadPoolExecutor(max_workers=2) as pool:
            fill = pool.submit(self.service.suggest, "틈새")
            self.assertTrue(has_read.wait(2))
            edit = pool.submit(self.service.edit_phrase, "create", None, "틈새 책방", "서점")
            release.set()
            fill.result(timeout=3)
            edit.result(timeout=3)
        self.service.aggregate = original
        self.assertIsNone(self.cache.get(old_key), "변경 후 오래된 SET이 되살아나면 안 됨")
        self.assertIn("틈새 책방", [value["text"] for value in self.service.suggest("틈새")[0]])

    def test_invalid_edit_does_not_change_data_or_invalidate(self):
        before = self.service.list_phrases()["total"]
        for text in ("", "a" * 81, "성수동카페", "bad\ntext"):
            with self.subTest(text=text), self.assertRaises(ValueError):
                self.service.edit_phrase("create", text=text, kind="키워드")
        with self.assertRaises(LookupError):
            self.service.edit_phrase("delete", 999999)
        self.assertEqual(self.service.list_phrases()["total"], before)
        self.assertEqual(self.cache.deletes, [])


class AdminHttpTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.cache = Cache()
        self.service = SearchService(Path(self.temp.name) / "admin.db", self.cache, event_count=1000)
        self.server = SearchHttpServer(("127.0.0.1", 0), handler_for(self.service))
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.base = "http://127.0.0.1:" + str(self.server.server_port)
        self.token = self.call("/api/admin/session")[1]["token"]

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=2)
        self.temp.cleanup()

    def call(self, path, method="GET", data=None, headers=None):
        body = None if data is None else json.dumps(data).encode()
        values = {"Content-Type": "application/json", "X-Admin-Token": getattr(self, "token", "")}
        if headers:
            values.update(headers)
        request = Request(self.base + path, data=body, method=method, headers=values)
        try:
            response = urlopen(request, timeout=3)
        except HTTPError as error:
            response = error
        with response:
            raw = response.read()
            content = json.loads(raw) if "application/json" in response.headers["Content-Type"] else raw
            return response.status, content

    def test_crud_http_automatically_invalidates_suggestions(self):
        suggest = "/api/suggest?" + urlencode({"q": "틈새"})
        self.assertEqual(self.call(suggest)[1]["suggestions"], [])
        created = self.call("/api/admin/phrases", "POST", {"text": "틈새 책방", "kind": "서점"})
        self.assertEqual(created[0], 201)
        self.assertEqual(created[1]["invalidation"]["deleted"], 1)
        phrase = created[1]["phrase"]
        self.assertIn("틈새 책방", [row["text"] for row in self.call(suggest)[1]["suggestions"]])
        changed = self.call("/api/admin/phrases/" + str(phrase["id"]), "PUT", {"text": "틈새 새책방", "kind": "서점"})
        self.assertEqual(changed[0], 200)
        self.assertIn("틈새 새책방", [row["text"] for row in self.call(suggest)[1]["suggestions"]])
        self.assertEqual(self.call("/api/admin/phrases/" + str(phrase["id"]), "DELETE", {})[0], 200)
        self.assertEqual(self.call(suggest)[1]["suggestions"], [])

    def test_mutations_require_token_same_origin_and_json(self):
        payload = {"text": "틈새 책방", "kind": "서점"}
        self.assertEqual(self.call("/api/admin/phrases", "POST", payload, {"X-Admin-Token": ""})[0], 403)
        self.assertEqual(self.call("/api/admin/phrases", "POST", payload, {"Origin": "https://elsewhere.example"})[0], 403)
        self.assertEqual(self.call("/api/admin/session", headers={"Host": "elsewhere.example"})[0], 403)
        self.assertEqual(self.call("/api/admin/phrases", "POST", payload, {"Content-Type": "text/plain"})[0], 415)

    def test_admin_page_assets_and_pagination(self):
        for path in ("/admin", "/admin/", "/admin.js", "/admin.css"):
            self.assertEqual(self.call(path)[0], 200)
        first = self.call("/api/admin/phrases")[1]
        second = self.call("/api/admin/phrases?page=2")[1]
        self.assertEqual(len(first["phrases"]), 50)
        self.assertTrue(second["phrases"])
        self.assertFalse({row["id"] for row in first["phrases"]} & {row["id"] for row in second["phrases"]})
        self.assertEqual(self.call("/api/admin/phrases?page=-1")[0], 400)


if __name__ == "__main__":
    unittest.main()
