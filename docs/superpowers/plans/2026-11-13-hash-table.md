# Hash Table Implementation Plan

**Goal:** 문자열 키·값의 저장·조회·삭제, 체이닝, 자동 확장을 검증한다.
**Architecture:** `src/hash_table.c`가 데이터를 소유하고 공개 선언은 `include/hash_table.h`에 둔다. 호출자가 조회 복사본을 해제한다. 이 단계는 단일 스레드 전용이다.
**Tech Stack:** C11, GCC, Make, assert, AddressSanitizer, UndefinedBehaviorSanitizer.

## Constraints

- 초기 버킷 수 16, FNV-1a 64비트, 체이닝.
- 새 키 추가로 적재율 1.0을 초과하면 버킷 수 두 배.
- 빈 문자열 키·값 허용, NULL 인자는 오류. C 문자열이므로 내장 NUL은 지원하지 않는다.
- `HT_OK`, `HT_NOT_FOUND`, `HT_NO_MEMORY`, `HT_INVALID_ARGUMENT`를 구분한다.
- 초기화한 테이블에만 연산한다. 복사·직접 필드 수정은 하지 않는다.
- 메모리 할당 실패 시 기존 데이터는 보존한다.
- HTTP, 만료, NX, 동기화, 로그, 웹은 이번 작업에서 제외한다.

## Task 1: Test-first implementation

- [x] `include/hash_table.h`, `tests/test_hash_table.c`, `Makefile` 작성.
  인터페이스: `ht_init`, `ht_destroy`, `ht_set`, `ht_get`, `ht_delete`.
- [x] 임시 미구현 본체로 `make test`를 실행해 초기화 기대 실패를 확인한다.
- [x] `src/hash_table.c` 구현: 노드별 키·값 복사, 문자열 비교, 연결 해제.
  조회는 복사본을 반환하며 실패 시 출력 포인터는 NULL이다.
- [x] 새 항목의 메모리와 새 배열을 확보한 다음 기존 상태에 연결한다.
  확장은 기존 노드의 키를 다시 해싱하여 새 배열에 연결하고 이전 배열만 해제한다.
- [x] `make test`로 기본 기능, 충돌 리스트 삭제, 반복 확장, 복사본 수명,
  잘못된 인자, 할당 실패를 검증한다.
- [x] `make sanitize`로 같은 테스트의 메모리 접근과 누수를 검사한다.
- [x] README에 실행 명령과 현재 범위를 기록하고 코드 리뷰 후 결과를 설명한다.

실행 위치: Dev Container의 `/workspaces/Mini_Redis`.
검증 명령: `make test`, `make sanitize`.
예상 결과: 각 테스트 PASS, sanitizer 오류 없이 종료 코드 0.
