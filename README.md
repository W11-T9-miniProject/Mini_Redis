# Mini Redis

C로 구현한 독립 실행형 Redis 호환 캐시 서버와 테스트, 설계 문서, 상품 조회 웹 데모입니다.

## 구성

- **[c-mini-redis/](c-mini-redis/)**: C 서버 본 구현. AE 단일 스레드 이벤트 루프, RESP2, String/List/Set/Hash/Sorted Set, 필수 TTL, 후보 5개 근사 LRU, 제한 Lua EVAL, RDB 스냅샷.
- **[product-demo/](product-demo/)**: C 서버에 연결하는 Node.js 상품 조회 데모. 별도 npm 의존성 없이 실행합니다.
- **[설계 계획](c-mini-redis/docs/DESIGN.md)** 및 **[설계도](c-mini-redis/docs/architecture.mmd)**.
- **[자동 테스트](c-mini-redis/tests/)**, **[검증 보고서](c-mini-redis/reports/VERIFICATION.md)**, **[부하 측정 결과](c-mini-redis/reports/BENCHMARK.md)**.
- **[Cache-aside·Write-back 예제](c-mini-redis/examples/cache_patterns.py)**: mock 원본 저장소와 클라이언트 메모리 대기열을 사용합니다.
- **[학습 자료](c-mini-redis/docs/LEARNING.md)**.
- 루트의 `server.js`, `package.json`은 기존 Node.js 초안입니다. 사용법과 지원 범위는 [초안 README](README.node-prototype.md)에 있으며, C 과제 본 구현은 `c-mini-redis/`입니다.

## C 서버 실행

C 컴파일러와 Lua 라이브러리가 필요합니다. Apple Silicon Homebrew 기본 경로를 사용하며, 다른 환경에서는 `LUA_PREFIX`를 지정합니다.

```sh
cd c-mini-redis
make
./mini-redis --port 6380 --dir ./manual-data
```

다른 터미널에서 접속합니다.

```sh
redis-cli -h 127.0.0.1 -p 6380 --raw
```

```redis
SET name kim EX 60
GET name
TTL name
```

지원 명령과 Lua·RDB 호환 범위는 [C 서버 README](c-mini-redis/README.md)를 확인하세요. 모든 Redis 명령을 지원하는 구현은 아닙니다.

## 자동 테스트

```sh
cd c-mini-redis
python3 -m venv .venv
.venv/bin/pip install pytest
.venv/bin/pytest -q
```

ASAN/UBSAN:

```sh
make asan
MINI_REDIS_BINARY="$PWD/mini-redis-asan" ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 .venv/bin/pytest -q
```

Apple ASAN 환경에서는 leak detection을 비활성화합니다. 검사 범위와 한계는 검증 보고서에 기록했습니다.

## 부하 테스트

6380 포트의 서버가 실행 중인 상태에서:

```sh
cd c-mini-redis
python3 tools/benchmark.py --port 6380
```

localhost TCP 50개 연결, pipeline 16, 총 100만 요청, 전체 요청 256바이트 조건이며 성공·오류 건수와 처리량을 출력합니다. 처리량 합격 기준은 지정되지 않았습니다.

## 상품 조회 데모

C 서버를 먼저 빌드한 뒤:

```sh
cd product-demo
npm start
```

브라우저에서 http://127.0.0.1:3000 에 접속합니다. 데모용 C 서버는 기본 16380 포트입니다. 상세 동작 및 테스트는 [데모 README](product-demo/README.md)를 참고하세요.

## 포함 범위

원본 소스·문서·테스트·측정 결과를 포함합니다. 실행 파일, 디버그 심볼, 가상환경, 테스트 캐시, 개인 실습 및 데모의 RDB 스냅샷은 재생성 가능한 로컬 산출물이므로 업로드하지 않습니다.
