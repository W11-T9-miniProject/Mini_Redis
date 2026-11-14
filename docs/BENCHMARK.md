# 캐시 사용 전후 실측

2026-11-13 Windows, 같은 PC에서 C Mini Redis와 Python HTTP API/SQLite를 실행했습니다.
두 API는 같은 임시 SQLite 파일의 상품 1개를 조회하며 측정 중 수정하지 않았습니다.
호출당 TCP 연결을 생성하는 현재 Python MiniRedis 클라이언트를 그대로 사용했습니다.

조건: 회당 HTTP 요청 100개, 동시성 4, 워밍업 10개, off/on 각각 3회.
캐시 TTL은 5초이며 각 측정 구간은 약 0.5초입니다. 워밍업은 집계에서 제외했습니다.

| 모드 | 회차 | 성공 RPS | p50(ms) | p95(ms) | DB 조회 | 캐시 결과 | 오류 |
|---|---:|---:|---:|---:|---:|---|---:|
| off | 1 | 197.70 | 18.79 | 24.99 | 100 | BYPASS 100 | 0 |
| off | 2 | 182.09 | 21.10 | 29.84 | 100 | BYPASS 100 | 0 |
| off | 3 | 198.20 | 20.08 | 24.86 | 100 | BYPASS 100 | 0 |
| on | 1 | 187.06 | 21.33 | 24.88 | 0 | HIT 100 | 0 |
| on | 2 | 183.39 | 21.02 | 27.23 | 0 | HIT 100 | 0 |
| on | 3 | 190.94 | 20.10 | 27.36 | 0 | HIT 100 | 0 |

캐시가 DB 조회를 300회에서 0회로 줄이는 동작은 확인했습니다.
하지만 이 작은 SQLite 단건 조회에서는 응답 속도 개선이 나타나지 않았습니다.
RPS의 회차 중앙값은 off 197.70, on 187.06이며 측정 변동도 있습니다.
네트워크/연결/JSON/HTTP 처리 비용이 추가되므로 DB 호출 감소가 곧 속도 향상을 뜻하지 않습니다.

작은 규모의 짧은 측정이며 장시간 부하나 운영 환경 성능을 보장하지 않습니다.
다음 비교는 비싼 실제 쿼리, 큰 응답, 더 긴 측정 구간, 연결 재사용 등을 분리해 수행해야 합니다.
TTL보다 측정 시간이 길면 자연스럽게 MISS/DB 조회가 추가될 수 있습니다.

재현 방법: Mini Redis와 cache off/on API를 실행한 뒤 아래를 각각 3회 반복합니다.

```sh
python3 bench/http_bench.py --url http://127.0.0.1:8080/products/1 --requests 100 --concurrency 4 --warmup 10
python3 bench/http_bench.py --url http://127.0.0.1:8081/products/1 --requests 100 --concurrency 4 --warmup 10
```

로컬 측정 원본 JSON은 build/benchmark-results.json에 있으며 Git에서는 제외됩니다.
