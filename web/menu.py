"""Shared cache for one explicitly selected public cafeteria post."""
import json
import statistics
import threading
import time
from urllib.parse import urlsplit
from urllib.request import Request, build_opener, HTTPRedirectHandler

from .errors import AppError

POST_ID = 114707701
SOURCE_URL = f"https://pf.kakao.com/_xhzNjn/contents/posts/{POST_ID}?post_only=true"
ORIGIN_URL = f"https://pf.kakao.com/rocket-web/web/v2/profiles/_xhzNjn/posts/{POST_ID}"
TTL_SECONDS = 3600


def parse_menu(data):
    if not isinstance(data, dict) or data.get("id") != POST_ID:
        raise AppError("지정한 식단 게시글을 확인할 수 없습니다.", 502)
    title = data.get("title")
    if not isinstance(title, str) or not 1 <= len(title) <= 300 or "식단" not in title:
        raise AppError("식단 게시글 제목을 확인할 수 없습니다.", 502)
    media = data.get("media")
    if not isinstance(media, list):
        raise AppError("식단표 이미지가 없습니다.", 502)
    for image in media:
        if not isinstance(image, dict) or image.get("type") != "image":
            continue
        url = image.get("xlarge_url") or image.get("url")
        if not isinstance(url, str) or len(url) > 4096:
            continue
        try:
            parsed = urlsplit(url)
            valid = (parsed.scheme == "https" and parsed.hostname == "k.kakaocdn.net" and
                     parsed.port in (None, 443) and not parsed.username and not parsed.password)
        except ValueError:
            valid = False
        if valid:
            return {"post_id": POST_ID, "title": title, "image_url": url, "source_url": SOURCE_URL}
    raise AppError("식단표 이미지 주소를 확인할 수 없습니다.", 502)


class NoRedirect(HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        raise AppError("식단 원본 주소가 변경되었습니다. 연결을 확인해 주세요.", 502)


def fetch_menu():
    # This is Kakao's web endpoint, not a supported public API contract.
    try:
        req = Request(ORIGIN_URL, headers={"Accept": "application/json", "User-Agent": "Jungle-MiniRedis-Menu-Demo/1.0"})
        with build_opener(NoRedirect()).open(req, timeout=5) as response:
            raw = response.read(256 * 1024 + 1)
            if len(raw) > 256 * 1024:
                raise AppError("식단 원본 응답이 너무 큽니다.", 502)
        return parse_menu(json.loads(raw))
    except (OSError, ValueError, RecursionError) as exc:
        raise AppError("카카오 식단 원본을 가져오지 못했습니다. 잠시 후 다시 조회해 주세요.", 502) from exc


class MenuService:
    def __init__(self, store, key, fetcher=fetch_menu):
        self.store, self.key, self.fetcher = store, key, fetcher
        # Single Flask process: coalesce cold requests; serialize invalidation too.
        self.lock = threading.RLock()

    def get(self, use_cache=True):
        start = time.perf_counter()
        with self.lock:
            entry = self.store.get(self.key) if use_cache else None
            if entry is not None:
                menu, fetched = entry["menu"], entry["fetched_at_ms"]
                status = "HIT"
            else:
                menu = self.fetcher()
                fetched = int(time.time() * 1000)
                if use_cache:
                    self.store.put(self.key, {"menu": menu, "fetched_at_ms": fetched}, ttl_ms=TTL_SECONDS * 1000)
                status = "MISS" if use_cache else "BYPASS"
        return {"menu": menu, "cache": status, "elapsed_ms": round((time.perf_counter() - start) * 1000, 3),
                "server_now_ms": int(time.time() * 1000),
                "fetched_at_ms": fetched, "expires_at_ms": fetched + TTL_SECONDS * 1000 if use_cache else None,
                "ttl_seconds": TTL_SECONDS if use_cache else None}

    def clear(self):
        with self.lock:
            self.store.call({"op": "delete", "key": self.key})

    def benchmark(self):
        if not self.lock.acquire(blocking=False):
            raise AppError("다른 식단 요청을 처리 중입니다. 잠시 후 다시 실행해 주세요.", 429)
        try:
            uncached, cached, menus = [], [], []
            # First genuine origin sample also supplies the warm-up payload.
            first = self.get(use_cache=False)
            self.store.put(self.key, {"menu": first["menu"], "fetched_at_ms": first["fetched_at_ms"]}, ttl_ms=TTL_SECONDS * 1000)
            uncached.append(first["elapsed_ms"])
            menus.append(first["menu"])
            for i in range(5):
                if i:
                    sample = self.get(use_cache=False)
                    uncached.append(sample["elapsed_ms"])
                    menus.append(sample["menu"])
                hit = self.get()
                if hit["cache"] != "HIT":
                    raise AppError("측정 도중 캐시가 사라졌습니다. 다시 실행해 주세요.", 503)
                cached.append(hit["elapsed_ms"])
                menus.append(hit["menu"])
            def stats(values):
                return {"samples": len(values), "mean_ms": round(statistics.mean(values), 3),
                        "median_ms": round(statistics.median(values), 3), "min_ms": min(values), "max_ms": max(values)}
            return {"samples_per_mode": 5, "origin_requests": 5, "same_payload": all(m == menus[0] for m in menus),
                    "uncached": stats(uncached), "cached": stats(cached),
                    "measurement": "웹 서버 내부 처리 시간; 이미지 다운로드·브라우저 왕복 제외; 캐시 예열 저장 제외"}
        finally:
            self.lock.release()
