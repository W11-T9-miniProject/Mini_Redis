# 소스를 읽는 순서와 호출 흐름

현재 코어와 통신/API 코드의 TODO는 구현으로 교체했습니다.
읽을 때는 '왜 이 계층이 필요한지 → 함수 입력/출력 → 메모리 수명 → 테스트' 순서로 확인하세요.

## 1. common.h / common.c: 바이트와 소유권

MrSlice는 주소와 길이를 빌린 참조입니다. 복사하거나 free하지 않습니다.
MrBuffer는 소유한 출력 버퍼이며 mr_buffer_free로 해제합니다.
문자열 끝의 NUL에 의존하지 않으므로 키/값에 NUL이 포함될 수 있습니다.
mr_malloc/calloc/realloc은 OOM 테스트를 위해 한곳에 모았습니다.
mr_alloc_fail_after는 테스트에서만 설정하며 서버 실행 시 기본값 -1을 유지합니다.

## 2. hash_table.c: 키에서 버킷으로

ht_init → ht_hash → ht_find → ht_set → ht_delete → ht_resize → ht_destroy 순으로 읽습니다.

```text
key → FNV-1a 숫자 → 숫자 % capacity → buckets[index]
                                          ↓
                                      첫 노드 → 다음 노드
```

버킷은 첫 노드의 주소를 담는 칸입니다. 다른 키가 같은 버킷에 갈 수 있으므로
키 길이와 실제 바이트를 비교합니다. size는 노드 수, capacity는 버킷 수입니다.
새 키를 추가하며 적재율이 약 75%를 넘으면 자동 확장합니다. 작은 capacity에서는
정수 반올림이 있습니다. resize는 %의 기준이 바뀌므로 노드를 다시 분류합니다.

새 SET은 값 복사 → 노드/키 할당 → 필요하면 새 버킷 할당 → 연결 순서입니다.
모든 할당이 성공하기 전에 기존 노드를 파괴하지 않아 OOM 시 이전 값/TTL이 보존됩니다.

노드는 두 종류의 연결을 가집니다.

- next/bucket_prev: 같은 버킷의 검색 체인.
- scan_next/scan_prev: 모든 노드를 잇는 순환 목록.

검색 체인은 충돌 해결에, 순환 목록은 만료 검사의 진행 위치 유지에 필요합니다.
ht_remove_entry는 두 연결에서 노드를 분리하고 커서를 보정한 뒤 메모리를 해제합니다.
버킷 이중 연결은 이미 찾은 노드 삭제 시 긴 체인을 다시 찾지 않기 위해 사용합니다.

확인할 테스트: tests/test_hash_table.c. 실제 주소/next를 종이에 그려보세요.

## 3. store.c: 물리적 존재와 논리적 존재

ht_find는 노드가 있는지 찾고, find_live는 그 노드가 만료됐는지 추가 판단합니다.
now>=expires_at이면 정리한 뒤 없음으로 취급합니다. 무기한은 -1입니다.
GET뿐 아니라 DEL/EXPIRE/TTL에도 같은 판정을 사용합니다.

SET PX의 보관 기간은 now_ms+ttl_ms로 바꿔 한 번에 저장합니다.
실행 중 now_ms는 단조 시계, 테스트에서는 직접 지정한 정수입니다.
정확히 만료 시각인 경우도 테스트하며 sleep으로 단위 테스트하지 않습니다.

접근 시 삭제만으로는 다시 조회되지 않는 키가 남습니다.
store_sweep는 순환 목록을 최대 budget개 검사해 이런 키도 회수합니다.
last_sweep_checked로 상한을 확인합니다. 바이트/실행 시간의 엄밀한 상한이 아니라
노드 검사 수 상한이며 큰 키의 해시 계산 등에는 추가 비용이 있습니다.
버킷 재배치 중에도 순환 목록은 유지되며, 삭제는 커서를 보정합니다.

확인할 테스트: tests/test_store.c.

## 4. resp.c: 바이트를 명령 인자로 바꾸기

```text
*3 CRLF $3 CRLF SET CRLF $4 CRLF name CRLF $3 CRLF kim CRLF
                              ↓
             argc=3, args=[SET, name, kim]
```

read_length가 마커/숫자/CRLF와 상한을 검사합니다.
resp_parse는 배열을 읽고 각 bulk payload의 시작/길이를 MrSlice로 반환합니다.
값 안의 CRLF는 길이에 포함되므로 구분자로 해석하지 않습니다.

args는 입력 버퍼를 빌립니다. command_execute를 끝내기 전에 입력을 memmove/realloc하면
안 됩니다. MR_NEED_MORE면 다음 recv까지 그대로 두고, 성공하면 consumed만큼만 버립니다.
입력에 여러 명령이 있어도 첫 명령의 길이만 consumed로 반환합니다.

응답 함수는 새 MrBuffer를 만들어 반환합니다. 빈 값은 $0, 없는 값은 $-1입니다.
확인할 테스트: tests/test_resp.c.

## 5. command.c: 문법과 실행 의미 연결

word → integer → command_execute 순서로 읽습니다.
RESP가 형식상 정상이어도 미지원 명령/인자 수/숫자/옵션은 여기서 거부합니다.

```text
GET → store_get → 유효한 값 → resp_bulk
                → 없음     → resp_null
DEL → store_delete → 삭제됨 → resp_integer(1)
                   → 없음 → resp_integer(0)
```

오류 응답 생성 성공도 MR_OK입니다. RESP로 표현한 명령 오류와 메모리 할당 실패를
구분해야 합니다. SET/DEL/EXPIRE는 응답 메모리를 미리 확보한 뒤 변경을 수행합니다.
하지만 응답 생성 후에도 네트워크가 끊길 수 있어 클라이언트가 실행 여부를 항상
알 수 있는 것은 아닙니다. 이것은 네트워크 API의 재시도 설계와 이어지는 문제입니다.

확인할 테스트: tests/test_command.c.

## 6. server.c: 하나의 이벤트 루프

```text
main → server_run
       ├─ store_init (전체 클라이언트가 공유할 단 하나의 Store)
       ├─ open_listener (socket/non-blocking/bind/listen)
       └─ 반복
           ├─ 남은 pipeline 이어서 실행
           ├─ poll/WSAPoll로 준비된 연결 대기
           ├─ accept_clients
           ├─ read_client → process_input
           ├─ flush_client
           └─ store_sweep
```

Client 하나가 fd/입력/출력/전송 진행 상태를 소유합니다. 연결 종료 시 close_client가 회수합니다.
Store는 연결마다 생성하지 않습니다. A가 저장한 값을 B도 조회할 수 있어야 합니다.

reserve는 버퍼를 필요한 만큼 확장하며 실패하면 원래 포인터를 보존합니다.
process_input은 최대 64개 명령만 실행하고, 남은 것은 pending_commands로 표시합니다.
남은 명령은 새 수신 이벤트가 없어도 다음 반복에서 실행합니다.

read_client는 현재 준비된 일부 바이트만 받습니다. recv 경계는 명령 경계와 무관합니다.
flush_client는 최대 64 KiB를 보내고 output_sent로 이어 보낼 위치를 저장합니다.
출력이 있을 때만 POLLOUT를 감시해 빈 출력 큐 때문에 바쁘게 반복하지 않습니다.
읽기만 종료한 클라이언트도 이미 보낸 요청의 응답을 받을 수 있게 처리합니다.

요청 형식 오류는 고정 오류를 큐에 넣고 전송 후 종료합니다.
출력 대기량 상한/OOM/연결 오류는 해당 연결만 닫습니다.
signal handler는 종료 플래그만 설정하고 실제 free/close는 루프 밖에서 수행합니다.

동시성은 저장소 변경을 한 스레드에서 차례로 실행해 다룹니다.
블로킹 통신이나 무제한 작업을 루프 안에 넣으면 전체 클라이언트가 기다리게 됩니다.
확인할 테스트: tests/integration/test_server.py. 사용자가 시작한 Windows 서버에서 통과했습니다.

## 7. Python 클라이언트와 HTTP API

mini_client.encode → TCP sendall → read_reply가 C 서버의 RESP API를 감쌉니다.
호출마다 연결해 간단하지만 연결 비용이 있습니다. 소켓을 공유하지 않아 API 스레드
간 충돌은 없으며, 성능 확장 시 connection pool을 별도로 설계할 수 있습니다.

api.read_cached는 GET → HIT이면 반환 → MISS이면 DB → SET PX 5000 → 반환 순서입니다.
캐시 장애는 BYPASS로 표시하고 DB를 사용합니다. DB 자체의 오류까지 숨기지 않습니다.
write_cached는 DB 커밋 후 DEL합니다. 삭제 실패는 통계에 남기며 원본 DB 변경은 유지합니다.
GET /stats로 DB 호출 수와 캐시 결과를 관찰할 수 있습니다.

DB 조회 도중 다른 요청이 수정하면 오래된 값이 나중에 캐시에 채워질 수 있습니다.
TTL로 오래된 값의 기간을 제한하지만 강한 일관성을 보장하지는 않습니다.
버전 키/락/재삭제 등을 추가한다면 어떤 순서의 경쟁을 해결하는지 별도로 검증해야 합니다.

확인할 테스트: tests/test_python.py(소켓 없는 단위), tests/integration/test_api.py(실제 통신).

## 8. 측정과 선택 과제

bench/http_bench.py는 실제 HTTP 응답만 성공으로 집계합니다.
워밍업과 측정 구간을 구분하고 /stats 전후 차이로 DB 조회 감소를 확인합니다.
캐시 off/on은 동일 DB/응답/부하로 3회 이상 반복하세요.
warmup=0만으로 기존 캐시가 지워지지는 않습니다. 새 키/DEL/만료로 cold 조건을 만드세요.
작은 SQLite SELECT가 캐시보다 빠를 수 있으므로 결과에 맞춰 분석합니다.

영속성은 아직 구현하지 않은 선택 과제입니다. 확장할 때는 로그 포맷뿐 아니라
write/fsync/메모리 반영/성공 응답의 순서와 부분 레코드 복구를 정해야 합니다.
단조 시계 값을 파일에 그대로 저장하면 재시작 후 의미가 없습니다.
절대 만료 시각을 기록하고 복구 시 남은 기간으로 변환하는 설계가 필요합니다.
