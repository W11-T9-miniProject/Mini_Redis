# 과제 중점 항목: 구현과 웹 사용을 구분한 검증표

2026-11-14 전체 재점검에서는 필수 기능 구현을 확인했지만 통합 테스트가
19개 중 1개씩 Windows 소켓 오류로 실패했습니다. 아래의 개별 검증 기록을
전체 안정성 검증 완료로 해석하지 않습니다. 최신 결과는 VERIFICATION.md를 참고하세요.

## 현재 범위

| 요구사항 | 구현 | 검색 웹에서의 사용 | 검증 |
|---|---|---|---|
| 동시성 | C 저장소는 단일 이벤트 루프에서 명령을 순서대로 실행 | 여러 HTTP 요청이 C 서버에 연결. 같은 검색어의 동시 MISS는 Python prefix lock으로 합침 | C 다중 클라이언트 기능 테스트, 검색 HTTP 8요청에서 DB 조회 1회 |
| 만료된 값 요청 | GET/DEL/TTL/EXPIRE가 공통 만료 판정을 사용. 주기적 정리도 수행 | 후보를 SET PX 30000으로 저장, 만료 후 DB에서 재계산 | C 정확한 경계 단위 테스트, 검색 HTTP에서 실제 만료 후 MISS/재계산 |
| 외부 사용 API | TCP/RESP 명령과 Python MiniRedis 클라이언트 | Python HTTP 검색 서버가 별도 C 프로세스의 GET/SET API를 사용 | 실제 TCP/HTTP 연동 테스트 |
| 데이터 무효화 | DEL 명령, EXPIRE, 덮어쓰기 지원 | /admin에서 추천어 추가/수정/삭제 후 관련 접두어·초성 캐시 자동 DEL | 관리 HTTP CRUD→실제 C 키 삭제→다음 검색 새 후보 반영, 외부 DEL 테스트 |
| 다운 시 데이터 보존(선택) | 영속성/AOF/스냅샷 미구현 | 연결 실패 시 원본 SQLite로 fallback. C 캐시는 재시작하면 비어 있음 | fallback 테스트는 있음. 저장소 복구/내구성 검증은 없음 |

## 1. 동시성

src/server.c에서 하나의 Store를 공유하고 poll/WSAPoll 이벤트 루프 안에서만
command_execute와 store_sweep를 실행합니다. 두 스레드가 버킷 연결을 동시에 수정하지 않습니다.
이는 여러 명령을 묶은 트랜잭션 보장이 아닙니다. GET→계산→SET 사이는 다른 요청이 들어올 수 있습니다.

Python HTTP 서버는 여러 스레드를 사용합니다. examples/search_app.py의 prefix_locks는
같은 키의 캐시 미스 때 DB 집계가 중복 실행되는 문제를 줄이기 위한 별도의 장치입니다.
실제 C 서버를 연결한 검색 HTTP 8요청에서 MISS 1개/HIT 7개/DB 집계 1회를 확인합니다.
첫 실행에서 HTTP 타임아웃이 한 번 발생했고 동일 조건의 재현 진단/테스트 재실행은 통과했습니다.
이 검증으로 임의의 부하에서 동시성 문제가 전혀 없다고 일반화하지 않습니다.

## 2. 만료

src/store.c의 find_live는 expires_at_ms != -1 && now_ms >= expires_at_ms이면
노드를 제거하고 MR_NOT_FOUND를 반환합니다. 만료 경계는 tests/test_store.c의 가짜 시계로 검사합니다.
이벤트 루프는 요청이 없어도 제한된 노드 수만큼 store_sweep를 호출합니다.

웹은 30초 TTL을 사용합니다. 통합 테스트에서는 C EXPIRE 명령으로 테스트 키의 TTL을
1초로 줄여 만료까지 기다린 뒤, 다음 HTTP 자동완성이 이전 값을 돌려주지 않고 다시
DB를 조회하는 것을 확인합니다. 임의의 실제 웹 키를 만료시키지 않습니다.

## 3. 외부 API

C 서버는 TCP/RESP, 검색 웹은 HTTP입니다. Python 프로그램이 네트워크로 C 서버를
사용하므로 C 내부 함수 호출만으로 동작하는 프로젝트는 아닙니다.
기본 주소 127.0.0.1은 같은 PC의 다른 프로세스에서만 접근할 수 있습니다.
다른 PC에서 접속하는 구성은 바인딩/방화벽/인증 검토가 필요하며 이번 검증 범위에 없습니다.
Redis 전체 명령이나 모든 Redis 클라이언트와의 호환성을 보장하지 않습니다.

## 4. 명시적 무효화

화면의 '최근 검색 지우기'는 브라우저 기록만 지우며 C 캐시를 지우지 않습니다.
일반 사용자 화면에는 Redis 관리 버튼이나 기술 설명을 추가하지 않았습니다.
별도 /admin 화면에서 추천어를 편집하면 서버가 DEL을 자동 호출합니다.
외부 사용자도 MiniRedis.delete(key) 또는 RESP DEL 명령으로 저장 데이터를 무효화할 수 있습니다.

tests/integration/test_search_cache.py의 test_external_del_exposes_updated_source는
시험용 DB에 새 검색 후보를 넣습니다. 기존 캐시가 있을 때는 이전 결과가 HIT로 나오고,
외부 DEL 후에는 새 결과가 MISS로 반영되는지 확인합니다.
추가한 test_admin_http_crud_issues_real_del_automatically는 관리 HTTP API를 통해
추가/수정/삭제하고 실제 C 서버의 이전 키가 없어지는지, 다음 자동완성에 갱신되는지 검사합니다.
이름 변경은 이전/새 이름의 모든 관련 접두어·초성을 무효화합니다.
같은 prefix의 집계→SET과 편집을 같은 stripe 잠금으로 조정해 오래된 채우기가 되살아나지
않게 했습니다. DEL 실패는 큐에 보관하고 다음 검색에서 재시도하며 prefix 버전으로 오래된
키 사용을 막습니다. 상세 구현/단일 웹 프로세스 범위는 ADMIN.md를 참고하세요.

## 5. 장애와 영속성

SQLite 원본 데이터가 남아 있는 것과 C Mini Redis가 자기 데이터를 복구하는 것은 다릅니다.
현재 C 저장소는 RAM 전용이므로 정상 종료/강제 종료 후 데이터를 복구하지 못합니다.
웹은 서버 연결이 실패하면 DB로 우회하고, 서버가 돌아오면 요청을 통해 캐시를 다시 채웁니다.
이는 캐시 사용 서비스의 가용성 방안이며 Mini Redis 자체의 내구성 구현은 아닙니다.

선택 과제를 확장한다면 AOF/스냅샷과 복구를 구현한 뒤 부분 로그/강제 종료/만료 시각 복구를
검사해야 합니다. 현재 단계에서 선택 항목을 완료했다고 주장하지 않습니다.

## 실행할 검사

```sh
python -m unittest discover -s tests -p 'test_*.py' -v
node --test tests/test_autocomplete.cjs
# 별도 전용 C 서버가 실행 중이어야 함. MR_PORT로 포트 지정 가능.
python -m unittest discover -s tests/integration -p test_search_cache.py -v
```

기존 C 단위 테스트와 TCP 기능 테스트 명령은 README를 참고하세요.
