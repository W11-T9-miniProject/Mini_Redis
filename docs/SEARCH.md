# 틈: 검색 자동완성 웹

## 실행

1. 기존 C Mini Redis를 6379 포트에서 실행합니다.
2. PowerShell에서 scripts/start-search.ps1을 실행합니다.
3. http://127.0.0.1:8090 을 엽니다.

Python 명령으로 실행하려면 examples/search_app.py --port 8090을 사용합니다.
웹은 로컬 C 소켓 서버와 연결하므로 로컬 프로젝트로 제공했습니다.
원격 호스팅에서는 같은 방식으로 사용자의 localhost TCP 서버에 접속할 수 없습니다.

## 화면

- 장소/동네/분위기를 입력하는 자동완성 검색.
- 한글 조합 중에도 현재 표시된 값이 90ms 유지되면 조회. 다음 글자/공백을 기다리지 않음.
- 조합 여부는 Enter/방향키의 IME 동작 보호에만 사용하고 추천 응답 표시를 막지 않음.
- 성수동카페/성수카페/성수 카페의 띄어쓰기·동네 별칭을 동일하게 처리.
- 빈 입력의 최근 검색/추천은 별도 그룹, 글자 입력 후에는 서버 추천만 표시.
- 90ms debounce, AbortController와 요청 버전으로 이전 검색 결과 역전 방지.
- ↑/↓, Enter, Escape 및 / 검색 포커스 단축키.
- 동네/카테고리 필터, 추천순/이름순, 6개 단위 더 보기.
- 장소 상세 모달, 유사 카테고리 검색.
- 최근 검색/장소 저장은 localStorage. 오류 시 현재 창에서는 동작.
- 좁은 화면에서는 2열 카드와 가로 카테고리 목록으로 전환.
- 화면에 Redis/캐시/성능 지표 등 구현 설명은 표시하지 않음.

## 데이터 흐름

```text
web/teum/app.js
 → GET /api/suggest?q=성
 → SearchService.suggest: 정규화된 검색어의 해시로 키 생성
 → C Mini Redis GET
 → HIT면 JSON 후보 반환
 → MISS면 prefix lock → 재조회 → SQLite 검색 이력 GROUP BY
 → SET PX 30000 → JSON 후보 반환
```

Unicode NFKC/공백/대소문자를 정규화하고 한글 초성도 같은 기준으로 비교합니다.
match_key는 공백을 제거하고 지원하는 동네 별칭(성수동/연남동/한남동/연희동)을
기본 이름으로 바꿉니다. 아무 글자의 '동'을 일괄 제거하지 않습니다.
DB의 후보 인덱스, 요청의 캐시 키, 장소 검색 모두 이 기준을 공유합니다.
search_tokens는 성수카페를 성수/카페로 나눠 띄어쓰기 유무에 따른 결과 차이를 방지합니다.
이전 규칙의 빈 결과가 남지 않게 namespace를 v3로 바꾸고 시연 인덱스를 다시 생성했습니다.
LIKE의 %와 _는 escape하고 SQL 파라미터를 사용합니다.
검색어는 80자, 후보는 최대 8개입니다. 빈 결과도 저장해 반복 미스 집계를 방지합니다.
동일 prefix의 동시 MISS는 고정 lock stripe 64개로 합칩니다.
캐시 연결/저장 실패는 DB 결과를 반환하며 SET 실패 시 같은 DB 집계를 반복하지 않습니다.
데이터/알고리즘 버전이 바뀌면 캐시 namespace가 달라집니다.

장소 목록은 작은 컬렉션에서 필터링하고, 실제 비용이 발생하는 검색 후보의 이력 집계에
캐시를 적용했습니다. Python 메모리 딕셔너리로 운영 캐시를 대신하지 않습니다.

## API

| 경로 | 역할 |
|---|---|
| / | 웹 화면 |
| /api/config | 카테고리/동네/초기 추천어 |
| /api/suggest?q= | 검색 후보, X-Cache 응답 헤더 |
| /api/search?q=&category=&district=&saved=&sort= | 장소 검색/필터 |
| /api/stats | 개발자용 캐시/DB 조회 통계 |
| /admin | 추천 검색어 관리 화면 |
| GET /api/admin/session | 로컬 관리 요청 토큰 |
| GET/POST /api/admin/phrases | 추천어 목록/추가 |
| PUT/DELETE /api/admin/phrases/{id} | 추천어 수정/삭제 + 자동 DEL |

정적 파일은 웹 루트 밖 경로를 거부합니다. HTTP/1.1에서는 표준 chunked framing을 사용합니다.
이 환경에서 고정 길이 정적 응답의 뒷부분이 누락되는 현상을 관찰했고, chunked 전송에서는
파일과 수신 바이트가 일치하는 것을 검사했습니다. 보안 제품 설정은 변경하지 않았습니다.

## 시연 데이터와 이미지

장소 30개의 상호/설명은 가상입니다. 실제 영업 정보/리뷰/사용자 수로 표시하지 않습니다.
자동완성 집계용 100,000개 검색 이벤트는 고정 seed로 생성한 합성 데이터입니다.
인위적인 sleep을 넣지 않고 실제 SQLite GROUP BY/COUNT 비용을 측정합니다.
사진은 특정 가게의 실제 사진이 아니라 분위기를 위한 이미지입니다.

로컬에 저장한 이미지 출처:

- cafe.jpg: https://images.unsplash.com/photo-1501339847302-ac426a4a7cbb
- forest.jpg: https://images.unsplash.com/photo-1441974231531-c6227db76b6e
- books.jpg: https://images.unsplash.com/photo-1507842217343-583bb7270b66
- gallery.jpg: https://images.unsplash.com/photo-1579783902614-a3fb3927b6a5
- interior.jpg: https://images.unsplash.com/photo-1497366754035-f200968a6e72
- architecture.jpg: https://images.unsplash.com/photo-1511818966892-d7d671e672a2

이미지는 이미 저장돼 있어 실행 시 다시 다운로드하지 않습니다.
글꼴은 Google Fonts의 Noto Sans KR을 요청하며 연결이 없으면 시스템 글꼴로 표시됩니다.

## 테스트와 한계

tests/test_search.py 16개 테스트:
정규화/초성, MISS→HIT, 빈 결과, 오류 캐시, 장애 fallback, SET 실패 시 DB 중복 방지,
같은 prefix 동시 요청, 필터/저장 목록, HTTP 자동완성/크기 제한,
정적 파일 원본 바이트 일치, 경로 이탈 거부.
별칭/붙여쓰기의 동일 후보·캐시 공유·결과 일치, 이전 인덱스 재생성,
개인 검색이 서버 후보로 자동 등록되지 않는 점도 회귀 테스트에 포함합니다.

브라우저 자동 접근 호스트가 사용할 수 없었고, 네이티브 미리보기 승인도 만료돼
실제 화면의 시각적 검수와 브라우저 클릭/IME 상호작용 검증은 완료하지 못했습니다.
JS 문법 검사와 API/정적 파일 테스트를 수행했습니다.

이후 사용자가 보고한 '성 입력 후 다음 키를 눌러야 추천이 뜨는 문제'를 수정했습니다.
tests/test_autocomplete.cjs는 실제 app.js를 DOM 모형에서 실행해 compositionend 없는
한 글자 추천, compositionupdate, 오래된 응답 무시, 중복 debounce, Escape, 포커스 이탈을
검사합니다. Node.js가 있으면 node --test tests/test_autocomplete.cjs로 6개를 실행할 수 있습니다.
이 검사는 실제 Windows IME를 조작한 브라우저 테스트와는 구분합니다.

tests/integration/test_search_cache.py는 실행 중인 C 서버에 연결해 동시 HTTP 8요청의
DB 조회 합치기, 만료 후 재계산, 외부 DEL 후 변경한 DB 후보 반영을 검사합니다.
관리 API CRUD가 자동으로 실제 DEL을 발행하는 경로도 검사합니다.
시험용 DB/캐시 namespace를 사용하며 일반 웹 데이터는 변경하지 않습니다.
요구사항별 구현/한계는 [중점 항목 검증표](REQUIREMENTS.md)를 참고하세요.
추천어 편집은 별도 [관리 화면](ADMIN.md)에서 제공하며 일반 검색 화면에 기술 설명을 추가하지 않았습니다.

bench/search_bench.py는 화면과 분리된 측정 도구입니다.
같은 데이터를 cache on/off로 각각 실행하고 RPS/p50/p95/DB 조회를 비교하세요.
단일 PC의 HTTP 서버/부하 생성기/새 TCP 연결 비용도 포함되므로 일반적인 운영 성능을
보장하는 수치로 해석하면 안 됩니다. TTL보다 긴 측정에서는 MISS가 다시 생길 수 있습니다.

## 짧은 실측 기록

2026-11-14, Windows에서 같은 SQLite 파일/검색어 ‘성’, 워밍업 1회 후
100요청·동시성 4로 비교했습니다. 웹 화면에는 이 표를 표시하지 않습니다.

| 항목 | cache off | cache on |
|---|---:|---:|
| 성공 RPS | 179.66 | 225.67 |
| p50(ms) | 18.73 | 16.57 |
| p95(ms) | 37.79 | 20.03 |
| 측정 구간 DB 조회 | 100 | 0 |
| HIT | 0 | 100 |
| 오류 | 0 | 0 |

이번 짧은 측정에서는 검색 이력 집계의 재사용 효과가 나타났습니다.
단 한 번의 비교이며 장시간/대용량 트래픽 성능을 증명하는 결과는 아닙니다.
개발용 짧은 결과를 서비스의 보장 수치처럼 표시하지 않습니다.
