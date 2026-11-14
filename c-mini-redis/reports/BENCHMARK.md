# 본측정 결과

50개 localhost TCP 연결, pipeline 16, 요청 전체 256바이트(값 191바이트), 각 1,000,000건.

| 대상 | 성공 | 오류 | 시간(초) | 처리량(요청/초) |
|---|---:|---:|---:|---:|
| C Mini Redis | 1,000,000 | 0 | 1.436061 | 696,349 |
| Memcached | 1,000,000 | 0 | 1.220875 | 819,085 |

성공 100만 건·오류 0건 기준은 양쪽 모두 충족한다. 처리량 하한은 미지정이므로
기준 미달 여부는 판정 불가다. 각 1회 측정이며 통계적 우열을 주장하지 않는다.
요청 프로토콜과 클라이언트 파싱 비용이 달라 순수 서버 연산 성능 비교와는 다르다.
초기 버전 측정값 대신 최종 수정 O2 바이너리의 재측정값을 사용했다.

최종 바이너리 SHA256: `06ce65c8af7f7f286ab98a1bf8c184624a3dfc51fc92be468b1726552daf6cce`

환경·명령·제약은 [ENVIRONMENT.md](ENVIRONMENT.md), 원시 결과는
[mini-redis-benchmark.json](mini-redis-benchmark.json),
[memcached-benchmark.json](memcached-benchmark.json)에 기록했다.
