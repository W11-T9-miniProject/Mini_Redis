# Mini Redis HTTP API

## 실행

Dev Container에서 실행합니다. 현재 컨테이너에는 필요한 라이브러리를 설치했고,
새 환경도 Dockerfile의 `libmicrohttpd-dev`, `libcjson-dev`, `pkg-config`로 재현합니다.

```bash
make server
./build/mini-redis-server
```

기본 주소는 `127.0.0.1:8080`, 로그는 `data/mini-redis.aof`입니다.
프로그램 실행 중 터미널이 점유되는 것은 정상이며 `Ctrl+C`로 종료합니다.
기존 저장소에 같은 로그를 열 수 없으면 시작을 거부합니다.

인자로 포트와 로그 경로를 지정할 수 있습니다. 포트 0은 OS 자동 배정입니다.
부모 디렉터리는 먼저 생성해야 합니다. 기본 `data` 디렉터리는 서버가 생성합니다.

```bash
./build/mini-redis-server 8081 data/demo.aof
```

다른 Dev Container 터미널에서 확인합니다.

```bash
curl http://127.0.0.1:8080/health
curl -s http://127.0.0.1:8080/kv -H 'Content-Type: application/json' \
  -d '{"op":"set","key":"seat:A1","value":"reservation-123","nx":true,"ttl_ms":20000}'
curl -s http://127.0.0.1:8080/kv -H 'Content-Type: application/json' \
  -d '{"op":"get","key":"seat:A1"}'
```

Windows에서 사용하려면 VS Code의 Ports 탭에서 8080을 전달합니다.
devcontainer.json에도 8080 전달을 지정했고 다음 연결 시 적용됩니다.
저장·삭제 API에는 인증을 구현하지 않았으므로 로컬 연결로 사용합니다.
이후 자리 예약 웹 서버가 호출할 내부 API이며, 교육생이 직접 사용하는 화면과는 다릅니다.

## 요청

`GET /health`는 프로세스에 접속 가능한지 확인합니다. 디스크 쓰기 가능 여부를 보장하지는 않습니다.

저장소 명령은 모두 `POST /kv`로 UTF-8 JSON 객체를 보냅니다.
`Content-Type: application/json`이 필요합니다.

| op | 필수 항목 | 선택 항목 |
| --- | --- | --- |
| set | key, value | nx, ttl_ms 또는 expires_at_ms |
| get | key | 없음 |
| delete | key | 없음 |
| compare_delete | key, expected | 없음 |
| compare_set | key, expected, value, expires_at_ms | 없음 |

`key`, `value`, `expected`는 문자열입니다. 빈 문자열도 허용합니다.
`nx`는 참·거짓 값이며 true이면 유효한 키가 없는 경우에만 저장합니다.

시간 단위는 **밀리초**입니다. `ttl_ms`는 상대 기간, `expires_at_ms`는 Unix 절대 시각입니다.
0은 만료 없음입니다. 일반 set에서 둘 다 생략하면 기존 만료도 해제합니다.
두 시간 항목의 동시 지정, NX=true와 절대 시각의 동시 지정은 거절합니다.
시간 숫자는 0~9007199254740991의 정수를 허용합니다.

조건부 연산은 유효한 현재 값과 `expected` 문자열 전체가 일치하는 경우에만 변경합니다.
`compare_set`은 존재하지 않는 키를 생성하지 않습니다.

본문은 최대 65536바이트입니다. 모르는 항목·중복 항목·잘못된 자료형·잘못된 JSON,
유효하지 않은 UTF-8, 문자열 안의 이스케이프되지 않은 제어문자와 내장 NUL을 거절합니다.
정상적인 제어문자 이스케이프는 허용합니다.

## 응답

```json
{"status":"ok"}
```

조회 성공은 값의 복사본을 JSON 문자열로 반환합니다.

```json
{"status":"ok","value":"reservation-123"}
```

| HTTP | status 예 | 의미 |
| --- | --- | --- |
| 200 | ok | 성공 |
| 400 | invalid_json / invalid_argument | 잘못된 요청 |
| 404 | not_found / unknown_endpoint | 키 또는 API 경로 없음 |
| 405 | method_not_allowed | 허용되지 않은 메서드 |
| 409 | condition_not_met | NX 또는 비교 조건 불충족 |
| 413 | request_too_large | 본문 크기 초과 |
| 415 | unsupported_media_type | JSON Content-Type 필요 |
| 500 | clock_error / lock_error | 내부 오류 |
| 503 | out_of_memory / storage_io_error | 처리 불가 또는 저장소 쓰기 중단 |

응답 유실이나 저장소 I/O 오류는 과거 변경의 확정적 취소를 뜻하지 않습니다.
기존 선점 ID로 현재 상태를 조회해야 합니다. 로그 쓰기 오류 이후에는 재시작 전까지
변경 요청을 중단하고 읽기만 유지하는 저장소 정책이 그대로 적용됩니다.

## 검사

```bash
make test
make sanitize
make http-test
make http-sanitize
```

HTTP 테스트는 별도 임시 로그와 자동 배정 포트를 사용하고 서버를 종료합니다.
실제 `data` 파일을 변경하지 않습니다.
기본 연산, 만료·오래된 ID, 요청 검증, 라우팅·크기 제한,
100개 동시 NX 요청, 중간 연결 종료, 프로세스 강제 종료·복원,
청크 업로드를 검사합니다.

HTTP 라이브러리는 4개 작업 스레드, 연결 제한 256개, 연결 비활성 시간 제한 10초로
실행합니다. 저장소 데이터 연산은 여전히 하나의 공통 락으로 직렬화합니다.
종료 시 HTTP 처리를 먼저 정지하고 저장소를 해제합니다.

브라우저 자리 선택 화면, 사용자 인증·관리자 권한, API 응답 캐시·성능 비교는 후속 단계입니다.
