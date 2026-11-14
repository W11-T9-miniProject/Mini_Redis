# Conditional Operations Implementation Plan

**Goal:** 현재 값이 예상 값과 같을 때만 삭제하거나 값·만료 시각을 변경한다.
**Architecture:** 공개 API가 공통 락을 획득하고 만료를 확인한 뒤 문자열을 비교한다.
조건이 맞으면 내부 노드 삭제/값 교체 함수를 사용하고 락을 해제한다.
이미 락을 보유하므로 공개 저장·삭제 함수를 내부에서 다시 호출하지 않는다.
**Tech Stack:** C11, pthread, 기존 assert 및 ASAN/UBSAN 테스트.

## Contract

- `ht_compare_delete(table, key, expected_value)`.
- `ht_compare_set_until(table, key, expected_value, value, expires_at_ms)`.
- 키 없음·만료는 HT_NOT_FOUND, 다른 값은 HT_CONDITION_NOT_MET.
- 비교는 저장된 전체 문자열을 대상으로 한다. 사용자 권한 검증은 웹 서버의 역할이다.
- 값 변경은 기존 항목만 대상으로 하며 키 없음·만료 시 새로 만들지 않는다.
- 만료 시각 0은 무기한, 양수는 Unix 밀리초. 음수는 잘못된 인자다.
- 변경 준비 실패 시 기존 값·만료 시각 유지. 항목 수는 값 변경으로 증가하지 않는다.
- 삭제·변경 모두 만료 확인부터 실제 변경까지 같은 락으로 보호한다.

## Steps

- [x] 조건부 삭제 테스트와 임시 일반 삭제 구현으로 잘못된 ID가 삭제되는 실패 확인.
- [x] 조건부 삭제 및 공유 노드 제거 함수 구현.
- [x] 조건부 변경 테스트에서 잘못된 ID가 덮어쓰는 실패 확인 후 구현.
- [x] 정상·만료·오래된 ID·충돌 리스트·잘못된 인자·시계/할당 실패 검사.
- [x] 100개 조건부 변경 요청 중 하나만 성공하고 그 값이 저장되는지 검사.
- [x] 취소와 확정 동시 요청에서 둘 중 하나만 성공하는지 검사.
- [x] `make test`, `make sanitize` 전체 19개 묶음 및 읽기 전용 리뷰 확인.
