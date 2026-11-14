import concurrent.futures
import unittest
from tests.test_web import WebTests
from web.app import create_app, AppError
from web.menu import MenuService, parse_menu

MENU={"post_id":114707701,"title":"11월 2주차 식단표","image_url":"https://k.kakaocdn.net/dn/example/img.jpg","source_url":"https://pf.kakao.com/_xhzNjn/contents/posts/114707701?post_only=true"}

class MenuTests(unittest.TestCase):
    setUpClass=classmethod(WebTests.setUpClass.__func__)
    start_redis=classmethod(WebTests.start_redis.__func__)
    stop_redis=classmethod(WebTests.stop_redis.__func__)
    admin=WebTests.admin

    def setUp(self):
        WebTests.setUp(self)
        self.calls=0
        def origin():
            self.calls+=1
            return dict(MENU)
        self.menu=MenuService(self.service.store,self.service.prefix+":menu",origin)
        self.app=create_app(self.service,self.secret,self.admin_key,menu_service=self.menu)
        self.app.config["TESTING"]=True

    def test_cache_shared_expiry_and_clear(self):
        a,b=self.app.test_client(),self.app.test_client()
        first=a.get("/api/menu").get_json()
        second=b.get("/api/menu").get_json()
        self.assertEqual(first["cache"],"MISS")
        self.assertEqual(second["cache"],"HIT")
        self.assertEqual(first["expires_at_ms"],second["expires_at_ms"])
        self.assertEqual(first["ttl_seconds"],3600)
        restored=MenuService(self.service.store,self.menu.key,lambda:self.fail("warm cache refetched"))
        self.assertEqual(restored.get()["cache"],"HIT")
        self.assertEqual(self.calls,1)
        raw=self.service.store.get_raw(self.menu.key)
        self.service.store.call({"op":"set","key":self.menu.key,"value":raw,"expires_at_ms":1})
        self.assertEqual(a.get("/api/menu").get_json()["cache"],"MISS")
        self.assertEqual(self.calls,2)
        self.assertEqual(a.post("/api/admin/menu/clear",json={}).status_code,403)
        self.assertEqual(self.admin().post("/api/admin/menu/clear",json={}).status_code,200)
        self.assertEqual(b.get("/api/menu").get_json()["cache"],"MISS")
        self.assertEqual(self.calls,3)

    def test_concurrent_fill_and_failure(self):
        with concurrent.futures.ThreadPoolExecutor(max_workers=10) as pool:
            results=list(pool.map(lambda _:self.menu.get(),range(10)))
        self.assertEqual(sum(r["cache"]=="MISS" for r in results),1)
        self.assertEqual(self.calls,1)
        self.menu.clear()
        def fail():raise AppError("원본 연결 실패",502)
        self.menu.fetcher=fail
        self.assertEqual(self.app.test_client().get("/api/menu").status_code,502)
        self.assertIsNone(self.service.store.get_raw(self.menu.key))

    def test_benchmark_permissions_counts_and_payload(self):
        public=self.app.test_client()
        for op in ("origin","benchmark"):
            self.assertEqual(public.post("/api/admin/menu/"+op,json={}).status_code,403)
        result=self.admin().post("/api/admin/menu/benchmark",json={})
        self.assertEqual(result.status_code,200)
        data=result.get_json()
        self.assertEqual(data["samples_per_mode"],5)
        self.assertEqual(data["origin_requests"],5)
        self.assertEqual(self.calls,5)
        self.assertTrue(data["same_payload"])
        self.assertEqual(data["cached"]["samples"],5)
        self.assertGreaterEqual(data["uncached"]["mean_ms"],0)
        self.assertEqual(public.get("/api/menu").get_json()["cache"],"HIT")
        self.assertEqual(self.admin().post("/api/admin/menu/benchmark",json={"samples":999}).status_code,400)

    def test_validation_and_page(self):
        valid={"id":114707701,"title":"11월 2주차 식단표","media":[{"type":"image","xlarge_url":MENU["image_url"]}]}
        self.assertEqual(parse_menu(valid),MENU)
        for bad in ({},{**valid,"id":1},{**valid,"title":""},{**valid,"media":[]},{**valid,"media":[{"type":"image","url":"http://evil.test/x"}]}):
            with self.assertRaises(AppError):parse_menu(bad)
        client=self.app.test_client()
        self.assertEqual(client.get("/menu").status_code,200)
        self.assertIn("https://k.kakaocdn.net",client.get("/menu").headers["Content-Security-Policy"])

    def test_changed_origin_invalidates_comparison(self):
        versions=iter([dict(MENU),{**MENU,"title":"수정된 식단표"},dict(MENU),dict(MENU),dict(MENU)])
        self.menu.fetcher=lambda:next(versions)
        self.assertFalse(self.menu.benchmark()["same_payload"])

    def test_bypass_does_not_replace_shared_cache(self):
        first=self.menu.get()
        self.menu.fetcher=lambda:{**MENU,"title":"변경된 식단표"}
        bypass=self.menu.get(use_cache=False)
        self.assertEqual(bypass["cache"],"BYPASS")
        self.assertIsNone(bypass["expires_at_ms"])
        self.assertEqual(self.menu.get()["menu"],first["menu"])

    def test_presentation_controls_and_public_permissions(self):
        public=self.app.test_client().get("/menu").data.decode()
        self.assertNotIn('id="menu-clear"',public)
        self.assertNotIn('어디에서 가져왔나요?',public)
        self.assertIn('id="menu-elapsed"',public)
        self.assertIn('id="cache-remaining"',public)
        self.assertIn('class="menu-overview"',public)
        self.assertNotIn('href="/present"',public)
        admin=self.admin().get("/menu").data.decode()
        self.assertLess(admin.index('id="menu-clear"'),admin.index('id="menu-image"'))
        self.assertNotIn('id="menu-origin"',admin)
        self.assertLess(admin.index('id="menu-notice"'),admin.index('id="menu-refresh"'))
        self.assertNotIn('class="notice" hidden',admin)
