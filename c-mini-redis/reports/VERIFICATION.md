# 독립 검증 보고서

최종 판정: 구현된 서버/API의 아래 검증 범위에서 통과. 발견한 실행 결함은 구현 에이전트가 수정했으며, 검증 에이전트는 서버·예제·테스트 코드를 변경하지 않았다. 확인 시각은 2026-11-13 16:34 KST이며 최종 검증 전후 `src/*.c`, `src/*.h`, 두 pytest 파일의 SHA-256이 동일했다.

## 실행 결과

| 실행 | 실제 결과 |
| --- | --- |
| 최종 소스를 별도 `/tmp/mini-redis-verify`로 O2 컴파일 | 경고·오류 없이 성공 |
| `MINI_REDIS_BINARY=/tmp/mini-redis-verify .venv/bin/pytest -q` | **67 passed in 8.21s** |
| 최종 소스를 별도 `/tmp/mini-redis-verify-asan`로 ASAN+UBSAN 컴파일 | 경고·오류 없이 성공 |
| `MINI_REDIS_BINARY=/tmp/mini-redis-verify-asan .venv/bin/pytest -q` | **67 passed in 8.80s** |
| `.venv/bin/pytest -q /tmp/test_mini_independent.py` | **3 passed in 0.25s**, ASAN+UBSAN 바이너리 사용 |
| `.venv/bin/python /tmp/mini_redis_verify.py` | 프로토콜 오류, TTL 정수 오류, 금지 while, 96MiB 응답 무결성·응답성 확인 |

빌드 명령:

```sh
cc -I/opt/homebrew/opt/lua/include/lua -I/opt/homebrew/opt/lua/include \
  -O2 -g -Wall -Wextra -Wpedantic src/*.c \
  -o /tmp/mini-redis-verify -L/opt/homebrew/opt/lua/lib -llua -lm
cc -I/opt/homebrew/opt/lua/include/lua -I/opt/homebrew/opt/lua/include \
  -O1 -g -Wall -Wextra -Wpedantic -fsanitize=address,undefined \
  -fno-omit-frame-pointer src/*.c -o /tmp/mini-redis-verify-asan \
  -L/opt/homebrew/opt/lua/lib -llua -lm
```

최종 테스트에는 approx-LRU에서 특정 최신 키의 생존을 보장하지 않는 계약에 맞춰 survivors 수 및 남은 값의 무결성을 확인하는 수정이 포함된다. 이 테스트 수정 뒤 O2/ASAN 전체 67개를 다시 실행한 결과가 위 표다.

67개 프로젝트 테스트는 string/list/set/hash/zset 정상·부재·만료·빈 컬렉션 삭제·잘못된 인자·잘못된 타입, TTL 상한 및 정수 경계, 바이너리 값, 분할 프레임, malformed RESP, pipeline16, 동시 NX 락, Lua 키 선언·허용 명령·오류 전 변경 유지, 스냅샷 fork/복구, 메모리 축출, 외부 cache-aside와 write-back 재시도/deadline/동시 덮어쓰기를 포함한다.

독립 추가 3개 테스트는 다음을 확인했다.

- Lua 명령 1,000회 제한, instruction 제한, 금지 문법·라이브러리, 9MiB GET 결과를 Lua로 옮길 때 OOM 오류를 4회 발생시킨 뒤 서버 응답성 및 원본 데이터 유지.
- 다른 연결이 GET을 반복하는 동안 Lua SET 400회를 실행해 관찰 값이 실행 전 0 또는 실행 후 400뿐임을 확인.
- 모든 타입을 포함하는 자체 생성 RDB를 공식 `redis-check-rdb`로 검사하여 **Checksum OK** 확인. 48시간 전 이름의 archive 삭제, 최신 archive 생성, CRC 1비트 손상 시 시작 실패 확인.

## 발견 및 재검증

| 발견한 문제 | 수정 및 최종 확인 |
| --- | --- |
| P1: 부분 전송한 출력 prefix를 회수하지 않아 응답 이력이 누적되고 메모리가 계속 증가 | `src/util.c:87`에서 append 전 compaction, `src/networking.c`에서 큰 출력 버퍼의 전송 완료 후 해제. 2MiB 값 GET 48개를 느리게 읽는 96MiB 응답 재현 통과. 최초 RSS 약 10MiB→106MiB, 최종 동일 재현 약 10MiB→27MiB. RSS는 allocator 보유 메모리도 포함하므로 누수 판정의 단독 근거로 사용하지 않음. |
| P2: RESP 길이·TTL에 선행 탭을 허용 | `src/util.c:56` strict decimal 검사. `*\t1` 및 `$\t4`는 protocol error 후 종료, SET EX 탭 정수는 명령 오류. |
| 요구 문법 밖의 finite while 허용 | `src/scripting.c`에서 while/repeat/until/goto 차단. finite while도 오류 확인. |
| EOF가 불완전 프레임 뒤에 오면 연결이 남음 | `src/networking.c:79`에서 EOF의 incomplete frame을 오류로 처리. 선행 PING 응답 후 오류·연결 종료 회귀 통과. |
| P2: LRU 표본이 해시 충돌 체인의 head만 선택하여 내부 키 제외 | `src/dict.c`의 dense live-key index와 `src/db.c`의 index 표본 5개 선택으로 수정. bucket16815에 충돌하는 `collision-575`/`collision-940` 반복 축출 회귀 통과. 표본 실패로 상한 초과 상태를 남길 수 있던 sparse-bucket 재시도도 제거됨. |
| P2: RDB의 원소 0개 LIST 복구 후 LPOP에서 NULL 역참조 | 수정 전 ASAN SEGV 재현. `src/rdb.c:262`에서 빈 컬렉션 RDB 시작 거부, LPOP 방어 추가. 최종 회귀 통과. |
| Lua OOM longjmp 시 임시 RESP 버퍼 정리 | LuaCtx scratch 소유권 및 EVAL 종료 정리 확인. 9MiB GET 반복 OOM 독립 테스트 통과. |

## 범위와 남은 외부 검증

AE 등록·삭제·timer와 단일 poll 루프, typed object/dict, 확률 skiplist, absolute-expiry RDB와 CRC64, Lua capability 검사는 소스 검토와 위 실행으로 확인했다. ZRANK는 level0 순회 O(N), dict는 고정 버킷으로 문서화되어 있다. RDB 입력 호환 범위는 자체 canonical v9 출력 형식이다.

24시간 보관은 과거 timestamp archive를 만든 가속 검사이며 실제 24시간 대기 검사가 아니다. 60초 기본 주기와 fork 경로는 소스 검토, 실행은 1초 설정으로 확인했다. ASAN/UBSAN 통과는 모든 누수·경쟁·sandbox 우회 부재를 증명하지 않는다. Apple ASAN의 leak detection은 지원 범위 밖이다.

정확히 256바이트·50연결·pipeline16·100만 건 성능 측정은 주 에이전트의 최종 측정 및 `reports/*benchmark*` 결과에 따른다. 독립 검증 에이전트는 성능 측정과 동시에 부하 벤치마크를 추가 실행하지 않았다. 합격 처리량의 숫자 기준은 사용자 요구에 지정되지 않았다.

Codesight 연결 도구가 없어 Codesight 내부 설계도 업로드·렌더링은 미완료다. TShark 실캡처는 `/dev/bpf0: Permission denied`로 미완료이며 실제 캡처 검증 통과로 간주하지 않는다. 이 두 외부 도구 작업을 전체 완료했다고 주장할 수 없다.
