"""동일 HTTP API를 캐시 off/on으로 각각 실행한 후 비교하는 측정 도구.

예: python3 bench/http_bench.py --url http://127.0.0.1:8080/products/1
표준 라이브러리 클라이언트/스레드 자체가 병목일 수도 있다.
"""

import argparse
from collections import Counter
from concurrent.futures import ThreadPoolExecutor
import json
import math
import time
from urllib.error import HTTPError, URLError
from urllib.request import urlopen
from urllib.parse import urlsplit, urlunsplit


def request(url):
    start = time.perf_counter()
    try:
        with urlopen(url, timeout=3) as response:
            body = json.loads(response.read())
            if response.status != 200 or not isinstance(body, dict) or "id" not in body:
                return None, "INVALID_RESPONSE"
            return (time.perf_counter() - start) * 1000, response.headers.get("X-Cache", "UNKNOWN")
    except (HTTPError, URLError, TimeoutError, OSError, ValueError) as exc:
        return None, type(exc).__name__


def read_stats(url):
    # 같은 API의 /stats는 측정 구간 전후에만 조회해 부하 요청에 섞지 않는다.
    parts = urlsplit(url)
    stats_url = urlunsplit((parts.scheme, parts.netloc, "/stats", "", ""))
    try:
        with urlopen(stats_url, timeout=3) as response:
            stats = json.loads(response.read())
            return stats if isinstance(stats, dict) and isinstance(stats.get("db_reads"), int) else None
    except (OSError, ValueError):
        return None


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--url", required=True)
    parser.add_argument("--requests", type=int, default=1000)
    parser.add_argument("--concurrency", type=int, default=10)
    parser.add_argument("--warmup", type=int, default=20)
    args = parser.parse_args()
    if args.requests < 1 or args.concurrency < 1 or args.warmup < 0:
        parser.error("requests/concurrency must be positive; warmup must be nonnegative")
    for _ in range(args.warmup):
        latency, source = request(args.url)
        if latency is None:
            parser.exit(1, f"Warmup failed: {source}\n")
    before = read_stats(args.url)
    start = time.perf_counter()
    with ThreadPoolExecutor(max_workers=args.concurrency) as pool:
        results = list(pool.map(lambda _: request(args.url), range(args.requests)))
    elapsed = time.perf_counter() - start
    after = read_stats(args.url)
    latencies = sorted(latency for latency, _ in results if latency is not None)
    sources = Counter(source for latency, source in results if latency is not None)
    errors = Counter(source for latency, source in results if latency is None)

    def percentile(fraction):
        return latencies[max(0, math.ceil(len(latencies) * fraction) - 1)] if latencies else None

    print(json.dumps({
        "url": args.url, "requests": args.requests, "concurrency": args.concurrency,
        "warmup": args.warmup, "elapsed_seconds": elapsed,
        "success_rps": len(latencies) / elapsed,
        "p50_ms": percentile(0.50), "p95_ms": percentile(0.95),
        "error_rate": (args.requests - len(latencies)) / args.requests,
        "cache_sources": sources, "errors": errors,
        "db_reads_delta": after["db_reads"] - before["db_reads"] if before and after else None,
    }, indent=2))
    raise SystemExit(1 if errors else 0)


if __name__ == "__main__":
    main()
