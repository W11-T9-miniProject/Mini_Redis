# Expiration Implementation Plan

**Goal:** 절대 만료 시각과 상대 TTL 저장, 접근 시 만료 삭제를 구현한다.
**Architecture:** 노드에 Unix 밀리초 만료 시각을 저장한다. 조회가 만료 노드를
제거하므로 `ht_get`은 변경 가능한 테이블을 받는다. 실제 서버 시계는
`clock_gettime(CLOCK_REALTIME)`으로 읽고 테스트에서만 링커 래퍼로 고정한다.
**Tech Stack:** C11, POSIX clock_gettime, 기존 GCC 테스트와 sanitizer.

## Rules

- 만료 시각 0은 무기한, 양수는 Unix 밀리초. 과거 시각도 저장 가능하다.
- `현재 시각 >= 만료 시각`이면 키를 제거하고 HT_NOT_FOUND로 취급한다.
- `ht_set_until(table, key, value, expires_at_ms)`는 절대 시각을 저장한다.
- `ht_set_ttl(table, key, value, ttl_ms)`는 양수 TTL을 절대 시각으로 변환한다.
- 일반 `ht_set`으로 덮어쓰면 만료를 해제한다. TTL 유지 기능은 추가하지 않는다.
- 접근하지 않은 만료 항목은 물리적 size에 포함된다. 확장 시 전체 정리하지 않는다.
- 시계 읽기 실패는 HT_CLOCK_ERROR, 잘못된 시간·덧셈 오버플로는 HT_INVALID_ARGUMENT.
- 살아 있는 항목의 변경에 필요한 메모리 할당 실패 시 기존 값과 기한을 유지한다.
- 만료, 재저장, 삭제는 같은 키 조회 경로를 사용한다.
- 이번 단계는 여전히 단일 스레드다. NX·락·로그·HTTP는 포함하지 않는다.

## Steps

- [x] 선언과 시계 제어 테스트를 추가하고 미구현 만료 검사가 실패하는지 확인.
- [x] 노드 만료 시각, 조회 시 정리, 상대 TTL 변환과 오류 처리를 구현.
- [x] 직전·정각·이후, 지연 삭제, 충돌 리스트, 확장, 재저장, 오류 테스트 실행.
- [x] `make test`, `make sanitize`로 기존 기능과 메모리 검사 확인.
- [x] 헤더·README 계약 갱신과 읽기 전용 코드 리뷰.

Run: Dev Container의 `/workspaces/Mini_Redis`에서 `make test`, `make sanitize`.
Expected: 기존 7개와 만료 4개 테스트 묶음 모두 PASS, 종료 코드 0.
