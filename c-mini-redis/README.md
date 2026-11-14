# C Mini Redis

독립 C11 TCP RESP2 서버입니다. 기존 상위 디렉터리 JavaScript 구현과 별도로 빌드·실행합니다. 네트워킹, AE 이벤트 루프, 키 dict, 타입 객체, sorted set skiplist, RDB, Lua가 각각 모듈로 나뉩니다.

## 실행

macOS에서 `brew install lua`와 C 컴파일러가 필요합니다. Lua 5.4/5.5를 지원하며 기본 헤더/라이브러리 위치는 Homebrew Apple Silicon 경로입니다. 다른 설치는 `make LUA_PREFIX=/설치경로`로 지정합니다.

```sh
cd c-mini-redis
make
./mini-redis --bind 127.0.0.1 --port 6379 --dir ./data --maxmemory 67108864
redis-cli -p 6379 SET example value EX 300
redis-cli -p 6379 GET example
```

`--maxmemory`는 키/객체 할당량(기본 64MiB)에 적용합니다. 프로세스 RSS, 소켓 버퍼, Lua VM은 별도입니다. 초과하면 임의 키 후보 5개의 최근 접근 시각으로 approximate allkeys LRU를 수행합니다. 1,024 연결, RESP 프레임 16MiB, 명령 4,096 인자, 연결 출력 대기 약16MiB 상한을 적용합니다. 신뢰하는 로컬 실습 환경을 기본으로 하며 인증/TLS/복제/클러스터는 없습니다.

## 명령과 TTL

모든 키에는 TTL이 있습니다. `SET key value [NX] EX seconds`에서 EX는 필수이며 1~300초입니다. EX와 NX 순서는 자유롭습니다. `EXPIRE key seconds`도 1~300초만 허용합니다. 컬렉션 생성 TTL은 300초이며 이후 수정은 TTL을 보존합니다. 빈 컬렉션은 키까지 삭제합니다. `TTL`은 남은 밀리초를 가장 가까운 초로 반올림하고 없는 키에는 -2를 반환합니다. lazy expiry와 AE 타이머의 active expiry를 모두 사용합니다.

| 명령 | 정상 응답 | 없는 키/원소 |
|---|---|---|
| SET key value [NX] EX n | +OK | NX 충돌 시 null bulk |
| GET key | bulk | null bulk |
| DEL key [key ...] | 삭제 키 수 integer | 0 |
| EXPIRE key n | 1 | 0 |
| TTL key | 남은 초 integer | -2 |
| FLUSHALL | +OK | +OK |
| KEYS * | key bulk 배열 | 빈 배열 |
| RPUSH key value [value ...] | 길이 integer | 생성 |
| LPOP key | bulk | null bulk |
| LRANGE key start stop | 양끝 포함 bulk 배열, 음수 인덱스 | 빈 배열 |
| LLEN key | 길이 integer | 0 |
| SADD/SREM key member [member ...] | 추가/삭제 수 integer | 생성/0 |
| SISMEMBER key member | 1/0 | 0 |
| SMEMBERS key | bulk 배열(순서 보장 없음) | 빈 배열 |
| HSET key field value [field value ...] | 새 필드 수 integer | 생성 |
| HGET key field | bulk | null bulk |
| HDEL key field [field ...] | 삭제 필드 수 integer | 0 |
| HGETALL key | field/value 교대 bulk 배열 | 빈 배열 |
| ZADD key score member [score member ...] | 새 원소 수 integer | 생성 |
| ZREM key member [member ...] | 삭제 수 integer | 0 |
| ZCARD key | 원소 수 integer | 0 |
| ZRANK key member | 0부터 시작 integer | null bulk |

`PING [message]`도 지원합니다. 다른 자료형 접근은 `WRONGTYPE`, 잘못된 인자·정수 오버플로·NaN score는 `ERR`입니다. ZADD는 모든 score를 검증한 뒤 변경하며 동일 score는 member의 바이너리 사전순입니다. malformed RESP에는 ERR 후 연결을 닫습니다. 정상 명령 오류 이후에는 다음 pipeline 요청을 처리합니다. 바이너리 키/값과 분할 수신을 지원합니다.

## 제한 Lua 및 안전한 락

`EVAL script numkeys key... arg...`에 실제 Lua를 사용합니다. 문자열/숫자/boolean/nil, local, if/else, bounded for, 함수와 return을 사용할 수 있습니다. `KEYS[i]`, `ARGV[i]`, `redis.call/pcall`만 제공하며 GET/SET/DEL/EXPIRE/TTL과 선언된 KEYS만 접근 가능합니다. 테이블 생성·일반 인덱싱·일반 속성 접근·메서드 호출·긴 괄호 문자열을 문법 검사로 거부하고 표준 라이브러리를 열지 않아 파일·네트워크·모듈 API가 없습니다. KEYS/ARGV는 읽기 전용 프록시입니다. pcall 오류는 테이블 사용 금지 제약 때문에 오류 문자열로 반환합니다.

Lua 메모리는 8MiB, 스크립트는 64KiB, 실행은 약 50ms 또는 1,000,000 VM instruction, Redis 명령은 1,000회로 제한합니다. hook 검사 주기는 1,000 instruction이므로 제한은 검사 사이에 소폭 초과할 수 있습니다. 실행 중 다른 클라이언트 명령을 처리하지 않습니다. 오류 이전 변경은 rollback하지 않습니다. while/repeat/goto는 문법 단계에서 거부하며 과도한 for 반복·재귀도 실행 제한으로 종료합니다.

```sh
redis-cli SET lock unique-owner-token NX EX 5
redis-cli EVAL "if redis.call('GET',KEYS[1]) == ARGV[1] then return redis.call('DEL',KEYS[1]) else return 0 end" 1 lock unique-owner-token
```

랜덤 토큰을 보유한 클라이언트만 해제할 수 있습니다. 만료 후 다시 획득한 다른 소유자의 락을 삭제하지 않습니다. 락 갱신·fencing은 이 예제 범위에 포함하지 않습니다.

## RDB

기본 60초마다 fork 자식이 `dump.rdb`를 임시 파일에 작성하고 fsync → rename → 디렉터리 fsync로 교체합니다. `snapshot-<milliseconds>.rdb` 하드링크를 생성하며 24시간 이전 아카이브를 정리합니다. 시작은 최신 `dump.rdb`에서 복구하며 이미 만료된 키는 제외합니다. SIGINT/SIGTERM 정상 종료 시 추가 저장합니다. AOF는 없으며 스냅샷 이후 비정상 종료 변경은 유실될 수 있습니다.

출력은 Redis RDB v9 string/list/set/hash/zset2 canonical encoding 및 CRC64와 호환되며 `redis-check-rdb`로 검증했습니다. 읽기는 이 서버가 출력하는 v9 canonical 타입에 제한됩니다. 임의의 최신 Redis RDB의 compact encoding, LZF, auxiliary 필드는 지원하지 않습니다. 손상/지원하지 않는 형식은 조용히 무시하지 않고 시작 실패로 알립니다. `--snapshot-interval 1`은 테스트용 주기 변경입니다.

## 테스트와 예제

```sh
python3 -m venv .venv
.venv/bin/pip install pytest
.venv/bin/pytest -q
make asan
MINI_REDIS_BINARY="$PWD/mini-redis-asan" ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 .venv/bin/pytest -q
python3 examples/cache_patterns.py --port 6379
python3 examples/cache_patterns.py --port 6379 --fail-db  # pending_failed 및 exit 1
```

Apple ASAN은 leak detector를 지원하지 않아 위 설정으로 ASAN/UBSAN을 실행합니다. 외부 예제는 표준 라이브러리만 사용합니다. cache-aside는 mock DB에서 cache miss를 채웁니다. write-back은 클라이언트 메모리 큐를 사용하고 DB 성공 항목만 삭제하며 실패는 재시도합니다. 세대 번호로 처리 중 덮어쓴 최신 값을 보존합니다. 정상 종료 deadline 내 미처리 건수/실패 시도 수/실패 exit code를 보고합니다. 비정상 종료 시 큐 유실은 허용하며 실제 DB 연결에는 자체 write timeout이 필요합니다.

벤치 재현 스크립트는 `tools/benchmark.py`, 실측은 `reports/`, 구조도는 `docs/architecture.mmd`, 설계는 `docs/DESIGN.md`를 참조하세요. 배치 응답 완료 지연은 개별 요청 지연과 구별해서 보고합니다.
