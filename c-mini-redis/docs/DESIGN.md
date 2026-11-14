# C Mini Redis 설계

## 설계 근거와 호환 범위

Redis 공식 [키 축출 설계](https://redis.io/docs/latest/develop/reference/eviction/)의
임의 표본 기반 근사 LRU와 [EVAL 키 전달 규칙](https://redis.io/docs/latest/commands/eval/)을
기준으로 삼는다. [Lua 실행 모델](https://redis.io/docs/latest/develop/programmability/eval-intro/)처럼
스크립트 실행 중 다른 명령을 끼워 넣지 않는다. TTL 필수, 최대 300초 및 제한 초과
스크립트의 강제 오류 반환은 이번 과제의 추가 제약이다. 모든 Redis 명령/API와의
완전한 호환을 뜻하지 않는다. 명령별 실제 호환 범위와 저장 형식은 구현 후 검증한다.

Codesight 연결 도구 및 대상 프로젝트가 현재 제공되지 않아 설계도 원본을
`architecture.mmd`에 저장한다. Codesight 내 업로드/렌더링은 아직 수행하지 않았다.

독립 C TCP 서버이며 기존 JavaScript 서버는 수정하지 않는다. RESP2 배열/벌크 문자열을 증분 파싱하고 단일 스레드 poll 기반 AE 이벤트 루프에서 연결별 입력·출력 버퍼를 관리한다. 한 연결의 pipeline은 순서대로 실행한다. 명령 실행 도중 다른 클라이언트 명령을 실행하지 않는다.

## 데이터 및 수명
해시 테이블 dict의 키는 바이너리 문자열, 값은 타입 태그가 있는 객체다. string, 연결 리스트, set/hash 해시 구조, score/member 순서의 sorted set을 제공한다. 모든 키는 만료 시각을 가지며 SET은 EX 1..300을 요구한다. 컬렉션 최초 생성은 300초, 변경은 기존 TTL 보존, 마지막 원소 삭제는 키 삭제다. 접근 시 lazy expiry와 이벤트 루프 주기 active expiry를 병행한다. 메모리 상한 초과 시 전체 키 중 임의 후보 5개의 최근 접근 시간을 비교하여 가장 오래된 키를 제거한다. 추정 메모리는 객체와 할당된 데이터 기준이며 프로세스 RSS와 다르다.

## 명령과 RESP
SET key value [NX] EX seconds, GET, DEL, EXPIRE, TTL, FLUSHALL, KEYS *, RPUSH/LPOP/LRANGE/LLEN, SADD/SREM/SISMEMBER/SMEMBERS, HSET/HGET/HDEL/HGETALL, ZADD/ZREM/ZCARD/ZRANK를 지원한다. 오류는 ERR/WRONGTYPE, 부재 값은 RESP null bulk, 부재 순위는 null bulk, 부재 TTL은 -2다. 범위 인덱스는 음수와 양끝 포함 의미를 따른다. 프로토콜 손상은 오류 응답 후 연결을 종료하며 정상 명령 오류는 연결을 유지한다. 크기/인자 개수 상한으로 자원 소진을 제한한다.

## 저장
60초마다 fork한 자식이 현재 시점 스냅샷을 Redis RDB v9 canonical 바이너리 형식(문자열/list/set/hash/zset2, CRC64)으로 임시 파일에 작성하고 fsync와 rename으로 교체한다. 스냅샷 보관은 24시간이며 시작 시 최신 dump.rdb에서 만료되지 않은 키만 복구한다. AOF는 제공하지 않는다. 스냅샷 사이 변경은 비정상 종료 시 유실될 수 있다.

## 제한 Lua
실제 Lua 런타임을 사용하되 기본 라이브러리와 OS/IO/package/debug 접근을 노출하지 않는다. 사용자 table 생성·접근을 제한하고 redis.call/pcall의 GET/SET/DEL/EXPIRE/TTL만 허용한다. KEYS로 선언한 키만 접근한다. 8MiB 전용 allocator, 50ms wallclock/instruction hook와 명령 개수 한도로 무한 루프를 중단한다. 단일 이벤트 루프에서 atomic하게 실행하며 실행 오류 이전 변경은 rollback하지 않는다. 토큰 락은 SET NX EX와 소유 토큰 비교 후 DEL 스크립트로 구현한다.

## 검증 및 외부 예제
구현 전에 pytest RESP 명령·만료·경계·오류·pipeline·동시 락 테스트를 작성한다. ASAN/UBSAN을 적용한다. 외부 Python 예제는 mock DB의 cache-aside와 프로세스 메모리 큐 기반 write-back을 제공한다. write-back 성공 항목만 제거하고 실패는 재시도하며 정상 종료 deadline에 남은 실패 건수와 실패 종료 상태를 알린다. 비정상 종료 큐 유실은 허용한다.

벤치마크는 localhost, 50 연결, pipeline 16, 총 1,000,000 요청, 요청당 256바이트를 재현 가능한 스크립트로 측정한다. 처리량과 지연의 정의 및 실측 환경을 기록한다. 이 설계에서 편차가 생기면 구현 문서에 정확하게 기록한다.


## 구현 확정 및 계약 보강

`src/ae.c/.h`는 파일 이벤트 등록·갱신·삭제, readiness callback, monotonic 주기 타이머를 제공한다. `server.c`는 조립과 cron, `networking.c`는 RESP 및 연결 상태, `dict.c`는 키 해시, `object.c`는 값 생명주기와 컬렉션 hash, `zskiplist.c`는 16레벨 확률 skiplist, `db.c`는 만료·LRU, `commands.c`는 명령, `scripting.c`는 Lua, `rdb.c`는 저장을 맡는다. 고정 버킷 chaining을 사용하며 Redis의 incremental rehash와 span 기반 O(log N) rank는 범위에 포함하지 않는다. ZRANK는 skiplist level0 순회로 O(N)이다.

명령별 정확한 RESP 반환은 README 표가 계약이다. EXPIRE도 1~300초만 허용하여 사용자 명시 범위를 우선한다. TTL은 `(남은 milliseconds + 500) / 1000` 반올림이다. ZADD는 전체 score 유효성을 먼저 확인하여 잘못된 중간 score가 부분 변경을 일으키지 않는다. 정수는 signed64 범위, score는 NaN 거부, 인자 수는 4,096, 프레임은 16MiB다. RDB 읽기는 자체 출력 canonical v9에 제한되며 CRC64 검증 실패는 시작 오류다. Redis `redis-check-rdb`의 Checksum OK를 확인했다.

Lua는 64KiB script, 8MiB allocator, 약50ms, 1,000,000 instruction, 1,000 command 제한이며 1,000 instruction마다 hook 검사한다. pcall 오류는 사용자 table 금지와 함께 쓰기 위해 오류 문자열로 전달한다. KEYS/ARGV 읽기 전용 proxy 외 일반 인덱싱/테이블/속성은 구조적 토큰 검사로 차단한다. 파일/네트워크/모듈 라이브러리는 열지 않는다.

최종 구현 검증: 일반 pytest 67개 및 ASAN+UBSAN 67개 통과. Apple ASAN은 leak detection을 지원하지 않아 detect_leaks=0이며 메모리 누수 검증을 수행했다고 주장하지 않는다. 스냅샷 fork/복구/만료제외/메모리 eviction 및 외부 write-back 재시도·deadline·in-flight 덮어쓰기 경쟁을 포함한다.


독립 검증 후 보강: canonical signed64 숫자만 허용하여 선행 탭/개행/0/+를 거부하고 부분 송신된 출력 prefix를 새 append 전에 회수한다. 큰 출력 버퍼는 완전 송신 후 해제한다. EOF의 불완전 RESP는 오류 후 연결 종료한다. LRU는 hash bucket head 대신 모든 키를 담은 dense keyslots 인덱스에서 후보5개를 뽑고 삭제 시 swap으로 O(1) 인덱스를 유지하여 충돌·sparse key의 후보 배제를 방지한다. index 할당도 maxmemory 추정에 포함한다. 복구 중 0개 원소 컬렉션은 불변식 위반으로 시작을 거부하고 LPOP도 빈 head를 방어한다.
