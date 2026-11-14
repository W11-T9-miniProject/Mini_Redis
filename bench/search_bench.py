"""자동완성 API 전용 측정. 웹에는 노출하지 않는 개발자 도구다.

실행 중인 검색 웹을 대상으로 사용한다. 비교할 때 cache off 웹을 별도 포트로 실행한다.
예: python bench/search_bench.py --url http://127.0.0.1:8090 --requests 200 --concurrency 8
"""

import argparse
from collections import Counter
from concurrent.futures import ThreadPoolExecutor
import json
import math
import time
from urllib.parse import urlencode
from urllib.request import urlopen


def read_json(url):
    with urlopen(url, timeout=5) as response:
        return json.loads(response.read()), response.headers.get("X-Cache", "UNKNOWN")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--url", default="http://127.0.0.1:8090")
    parser.add_argument("--requests", type=int, default=200)
    parser.add_argument("--concurrency", type=int, default=8)
    parser.add_argument("--prefix", default="성")
    args = parser.parse_args()
    if args.requests < 1 or args.concurrency < 1 or not args.prefix.strip():
        parser.error("positive requests/concurrency and nonempty prefix required")
    base = args.url.rstrip("/")
    url = base + "/api/suggest?" + urlencode({"q": args.prefix})
    read_json(url)  # 워밍업은 측정에서 제외한다.
    before, _ = read_json(base + "/api/stats")

    def request(_):
        start = time.perf_counter()
        try:
            data, source = read_json(url)
            if not isinstance(data.get("suggestions"), list):
                return None, "INVALID_RESPONSE"
            return (time.perf_counter() - start) * 1000, source
        except (OSError, ValueError) as exc:
            return None, type(exc).__name__

    start = time.perf_counter()
    with ThreadPoolExecutor(max_workers=args.concurrency) as pool:
        results = list(pool.map(request, range(args.requests)))
    elapsed = time.perf_counter() - start
    after, _ = read_json(base + "/api/stats")
    latencies = sorted(latency for latency, _ in results if latency is not None)
    errors = Counter(source for latency, source in results if latency is None)
    percentile = lambda fraction: latencies[max(0, math.ceil(len(latencies) * fraction) - 1)] if latencies else None
    print(json.dumps({
        "url": base, "prefix": args.prefix, "requests": args.requests, "concurrency": args.concurrency,
        "success_rps": len(latencies) / elapsed, "p50_ms": percentile(.5), "p95_ms": percentile(.95),
        "db_reads_delta": after.get("db_reads", 0) - before.get("db_reads", 0),
        "cache_sources": Counter(source for latency, source in results if latency is not None),
        "errors": errors,
    }, ensure_ascii=False, indent=2))
    raise SystemExit(1 if errors else 0)


if __name__ == "__main__":
    main()
