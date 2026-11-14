# Mini Redis (C)

직접 구현한 해시 테이블을 TCP 서버로 노출하는 작은 key-value 저장소입니다.
저장소 핵심은 C11, API 사용 예제와 측정 도구는 Python 표준 라이브러리로 작성했습니다.

## 검색 자동완성 웹 — 틈

장소·동네·분위기로 검색하는 웹을 추가했습니다. 화면에는 기술 설명이나 성능 패널을
넣지 않고 실제 장소 탐색 서비스처럼 구성했습니다. 데이터는 시연용 가상 컬렉션입니다.

```powershell
# Mini Redis를 먼저 켜둔 뒤, 별도 PowerShell에서
.\scripts\start-search.ps1
```

브라우저에서 **http://127.0.0.1:8090** 을 엽니다.
자동완성, 한글 초성(예: ㅅㅅ), 동네/카테고리 필터, 최근 검색,
장소 저장, 상세 보기, 더 보기를 제공합니다. 저장/최근 검색은 브라우저에 보관합니다.

```sh
# Linux 또는 직접 Python을 사용할 때
python3 examples/search_app.py --port 8090
python3 -m unittest discover -s tests -p test_search.py -v
```

자동완성 집계는 C Mini Redis에 30초간 저장하며 장애 시 SQLite로 우회합니다.
측정은 제품 UI와 분리했습니다. 개발자용 통계는 /api/stats, 실행 도구는 아래와 같습니다.

```powershell
# 비교용 웹을 새 터미널에서 실행
.\scripts\start-search.ps1 -Port 8091 -WithoutCache
# python 명령 사용 가능한 터미널에서 각각 비교
python bench/search_bench.py --url http://127.0.0.1:8090
python bench/search_bench.py --url http://127.0.0.1:8091
```

구조/데이터/검증 범위는 [검색 웹 안내](docs/SEARCH.md)에 기록했습니다.

**현재 검증 상태:** Windows 빌드, C 단위 테스트 18개, Python 단위 테스트 8개,
실제 TCP/HTTP 기능 테스트 15개가 통과했습니다.
Avast Behavior Shield 차단으로 첫 실행 검증이 중단됐지만, 사용자가 파일을 복원하고
서버를 시작한 뒤 실행 중인 서버를 대상으로 기능 테스트 전체 통과를 확인했습니다.
이는 기능 검증 결과이며 Avast 탐지가 오탐이라는 판정을 뜻하지는 않습니다.

## Windows 빠른 시작

PowerShell에서 저장소 루트 기준입니다. Visual Studio C++ 도구와 Windows SDK가 필요합니다.

```powershell
# 소스 빌드 + C 단위 테스트
.\scripts\build-windows.ps1 -Test

# 서버 실행. 인자를 생략하면 기본 포트 6379 사용.
.\build\mini-redis.exe
```

다른 터미널에서 명령을 보내는 방법입니다. Python 3.10 이상이 필요합니다.

```powershell
python examples/mini_client.py PING
python examples/mini_client.py SET name kim
python examples/mini_client.py GET name
python examples/mini_client.py SET message hello PX 5000
python examples/mini_client.py TTL message
python examples/mini_client.py DEL name
```

PING은 PONG, SET은 OK, GET은 저장된 값 또는 (nil)을 출력합니다.
서버 터미널에서 Ctrl+C로 종료합니다. 데이터는 메모리에만 보관되므로 종료 시 사라집니다.

이 PC에서 python이 PATH에 없다면 실제 Python 실행 파일 경로로 대체하세요.
빌드된 파일은 build/ 아래에 있으며 Git에 포함하지 않습니다.

## Linux / WSL

```sh
make
make test
make sanitize
./build/mini-redis 6379
python3 examples/mini_client.py PING
```

Linux용 소켓 경로는 소스에 포함했지만 이 환경에서는 WSL 실행 권한 오류로 검증하지 못했습니다.
Windows는 Winsock/WSAPoll, Linux는 POSIX sockets/poll을 사용합니다.

CMake도 지원합니다.

```sh
cmake -S . -B build/cmake
cmake --build build/cmake --config Debug
ctest --test-dir build/cmake -C Debug --output-on-failure
```

Visual Studio 생성기라면 실행 파일은 build/cmake/Debug/에 생성됩니다.
이 환경에서는 CMake/MSBuild의 사용자 SDK 디렉터리 접근이 거절되어 직접 MSVC로 빌드했습니다.

## 요청이 처리되는 순서

```text
클라이언트
 → TCP 수신 버퍼 (server.c)
 → RESP 바이트 해석 (resp.c)
 → 명령 검증·선택 (command.c)
 → 만료 규칙 적용 (store.c)
 → 버킷/충돌 체인 검색·변경 (hash_table.c)
 → RESP 응답 생성 (resp.c)
 → 출력 큐에 보관하고 TCP 전송 (server.c)
```

모든 C 저장소 접근은 하나의 이벤트 루프에서 일어납니다.
클라이언트마다 입력/출력 버퍼는 별도이고 Store는 하나를 공유합니다.
전체 함수 설명과 메모리 수명은 [코드 읽는 순서](docs/ROADMAP.md)를 참고하세요.

## 지원 명령

| 명령 | 동작 |
|---|---|
| PING | PONG 반환 |
| SET key value | 저장/덮어쓰기, 기존 TTL 제거 |
| SET key value PX milliseconds | 양수 TTL과 함께 저장 |
| GET key | 조회, 없거나 만료됐으면 null |
| DEL key | 삭제 수 0 또는 1 |
| EXPIRE key seconds | 만료 설정, 0 이하는 즉시 삭제 |
| TTL key | 남은 초, 무기한 -1, 없는 키 -2 |

명령은 ASCII 대소문자를 구분하지 않습니다. 키/값은 구분합니다.
빈 값/빈 키/NUL/개행을 포함한 바이트열도 지원합니다.
지원하는 RESP2 형식은 bulk string 배열입니다. 자세한 [계약](docs/CONTRACT.md)을 확인하세요.

## 상품 API와 캐시

Mini Redis 서버를 켜둔 상태에서 다음을 각각 다른 터미널에서 실행합니다.

```powershell
python examples/api.py --port 8080 --cache off
python examples/api.py --port 8081 --cache on
```

```powershell
curl.exe -i http://127.0.0.1:8081/products/1
curl.exe -i http://127.0.0.1:8081/products/1
Invoke-RestMethod -Method Put -Uri http://127.0.0.1:8081/products/1 -ContentType application/json -Body '{"name":"mouse"}'
curl.exe -i http://127.0.0.1:8081/products/1
curl.exe http://127.0.0.1:8081/stats
```

최초 조회는 MISS, 이후 조회는 HIT가 기대 동작입니다.
수정은 DB 커밋 후 DEL로 캐시를 무효화합니다. TTL은 5초입니다.
캐시 연결 실패/타임아웃은 DB로 fallback하며 X-Cache: BYPASS로 표시합니다.
/stats는 DB 조회 수, HIT/MISS/BYPASS, 무효화 실패 수를 반환합니다.
SQLite 파일은 예제 실행 시 생성합니다. --db로 별도 파일을 지정할 수 있습니다.

DB 수정과 동시 캐시 채우기 사이에는 오래된 값이 다시 저장되는 경쟁 조건이 남습니다.
이 예제의 cache-aside + TTL은 강한 일관성을 보장하지 않습니다.

## 테스트

```powershell
# C 실행 파일이 필요 없는 Python 프로토콜/캐시 흐름 단위 테스트
python -m unittest discover -s tests -p test_python.py -v

# 전용 서버/임시 SQLite를 생성하고 종료하며 실제 통신 검증
python tests/run_all.py
```

Linux에서는 python 대신 python3를 사용하면 됩니다.
run_all.py는 C 단위 테스트와 TCP/HTTP 기능 테스트를 실행합니다.
이미 사용하는 서버를 종료하지 않고 임시 포트를 사용합니다.
직접 실행한 전용 서버를 대상으로 테스트하려면:

```sh
python3 -m unittest discover -s tests/integration -v
```

C 단위 테스트는 충돌 삭제, 2,000개 키, 메모리 할당 실패 롤백,
TTL 경계, 긴 충돌 체인의 제한된 만료 검사, 파서 경계, 명령 오류 등을 확인합니다.
기능 테스트는 pipeline, 반쪽 요청, half-close, 여러 클라이언트,
느린 수신자, HTTP 캐시/무효화/장애 fallback을 다룹니다.
현재 통과/미검증 구분은 [검증 기록](docs/VERIFICATION.md)에 적었습니다.

## 성능 비교

```sh
python3 bench/http_bench.py --url http://127.0.0.1:8080/products/1 --requests 1000 --concurrency 10
python3 bench/http_bench.py --url http://127.0.0.1:8081/products/1 --requests 1000 --concurrency 10
```

동일 요청의 성공 RPS, p50/p95, 오류율, HIT/MISS/BYPASS와 DB 조회 수 변화량을 비교합니다.
Windows에서 같은 SQLite DB로 3회씩 비교한 [실측 결과](docs/BENCHMARK.md)를 기록했습니다.
작은 SQLite 단건 조회에서는 캐시 네트워크/연결 비용으로 더 느릴 수 있습니다.

## 한계

- RAM 저장소입니다. 영속성/AOF, 복제, 클러스터는 구현하지 않았습니다.
- 전체 저장소 메모리 상한/eviction은 없습니다. 요청/연결별 버퍼 상한은 있습니다.
- 인증/TLS가 없습니다. 기본은 127.0.0.1이며 다른 호스트에 노출하려면 별도 검토가 필요합니다.
- 외부 접속 주소는 mini-redis [port] [bind-address]로 지정합니다.
- FNV-1a는 학습용 해시이며 악의적으로 구성한 충돌에 대한 방어는 없습니다.
- 단일 명령 실행은 직렬화하지만 여러 명령 묶음의 트랜잭션은 지원하지 않습니다.
- Redis 일부 명령만 구현하며 Redis 전체 호환성을 보장하지 않습니다.
