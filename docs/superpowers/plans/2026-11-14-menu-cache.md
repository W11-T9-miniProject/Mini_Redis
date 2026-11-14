# Shared weekly menu cache implementation plan

Goal: Cache the specified Kakao cafeteria post response in the existing C Mini Redis for 3600 seconds and compare genuine uncached requests against warm hits.
Architecture: web/menu.py owns a fixed upstream JSON fetch, strict response validation, one shared key and a per-process lock for cache fills/invalidation. web/app.py exposes read-only public menu and authenticated admin controls. Existing reservation data and server remain unchanged.
Stack: Python standard library, existing Flask and MiniRedis client, existing HTML/CSS/JS.

Constraints: Only post 114707701 in channel _xhzNjn; metadata/image URL cached, image bytes fetched separately by browser. No OCR/latest-post discovery. No artificial delay. No upstream load tests: five sequential samples per mode, one benchmark at a time. Network failures must be explicit, never cached as valid data. TTL does not renew on hit. Data remains shared across browsers. Kakao web endpoint is undocumented and can change.

- [x] Add tests/test_menu.py using real isolated C server and injected origin to test miss/hit, TTL expiry, manual invalidation, failure, cross-client sharing, concurrent miss collapse, validation, permissions and benchmark counts/equal payload.
- [x] Run failing tests before implementation.
- [x] Add web/menu.py with MenuService(store,key,fetcher), get(), clear(), benchmark(). Fixed upstream JSON fetch has timeout, response size bound, field and image host validation. Lock covers recheck/fetch/write and clear. Benchmark reports mean/median/min/max, actual origin calls and equality; all times measured, no synthetic latency.
- [x] Wire GET /menu, GET /api/menu and authenticated POST /api/admin/menu/{origin,clear,benchmark}; reuse origin/body/admin validation. Add narrowly scoped CSP image domain k.kakaocdn.net.
- [x] Add dedicated menu template and JS, reuse existing visual style; origin link, zoomable image, cache/source badge, timing, TTL explanation, admin controls and result table. Add menu links to seat navigation.
- [x] Run tests, existing web/HTTP/core checks and JS syntax. Review identity preservation and cache thread safety.
- [x] Restart only owned Flask process, retain C server and tunnel. Verify public menu HIT/MISS, responsive view and administrator error controls without modifying seats. Record actual benchmark output and limitations in docs/menu-cache.md.

