# 검증 기록

2026-11-13, Windows / Visual Studio MSVC 14.41, /std:c11 /utf-8 /W4 /WX.

## 확인한 항목

- C 코어, 단위 테스트, Winsock 서버 빌드 성공. 컴파일 경고도 오류로 취급.
- C 단위 테스트 18개 통과.
- 이 테스트에는 2,000개 키 저장/조회/삭제, 각 할당 실패 지점의 롤백,
  정확한 TTL 경계, budget=1인 긴 충돌 체인 정리와 도중 resize/삭제가 포함됨.
- RESP의 binary/빈 키/길이 제한/INT64_MIN 응답/4,096개 임의 입력도 검사.
- 서버 실행 파일을 사용하지 않는 Python 단위 테스트 8개 통과.
  RESP 인코딩/응답/오류와 캐시 MISS/HIT, DB 커밋 후 무효화, 장애 fallback을 확인.
- Python 테스트에서 발견한 SQLite 연결 미종료를 수정.
  트랜잭션 종료와 연결 close를 분리해 finally에서 반드시 연결을 닫도록 변경.

## 실제 TCP/HTTP 검증

첫 TCP/HTTP 기능 테스트는 타임아웃으로 실패했고, 같은 작업 중 사용자가
Avast Behavior Shield의 IDP.Generic 차단을 보고해 실행 검증을 중단했습니다.
이후 사용자가 파일을 복원하고 기본 포트 6379로 서버를 시작했습니다.
그 서버를 대상으로 실제 기능 테스트 15개가 9.138초에 모두 통과했습니다.

- TCP 10개: CRUD/binary, TTL, 요청 분할/pipeline, half-close, 명령 오류,
  형식/크기 제한, 1 MiB를 넘는 정상 pipeline, 다중 클라이언트, 느린 수신자.
- HTTP 5개: DB 경로/오류 입력, TTL 만료, 손상된 캐시 복구, MISS→HIT→수정 후
  무효화와 DB 호출 수, 캐시 장애 시 조회/수정 fallback.
- 테스트가 띄운 HTTP 프로세스와 임시 SQLite는 테스트 종료 시 정리했습니다.
- 사용자가 시작한 Mini Redis 서버는 종료하지 않았습니다.
- 캐시 off/on 각각 워밍업 10회 후 요청 100개, 동시성 4로 3회씩 측정했습니다.
  오류 0, 캐시 on 측정 요청 300개 모두 HIT, DB 조회는 off 300회/on 0회입니다.
  수치와 해석은 BENCHMARK.md에 기록했습니다.

Avast 탐지가 오탐인지는 아직 판정되지 않았습니다. 이 기록은 기능 검증입니다.
사용한 cl.exe의 Authenticode 서명은 Microsoft Corporation / Valid로 확인했습니다.
이는 컴파일러의 서명 확인이며 생성한 프로그램의 안전성 판정은 아닙니다.

## 아직 확인하지 못한 항목

- Windows 서버에 대한 메모리 sanitizer 검사.
- Linux 빌드/ASan/UBSan: WSL 실행 권한 오류로 이 환경에서 실행 불가.

소스 코드, 빌드 스크립트와 테스트는 검토할 수 있도록 그대로 두었습니다.
