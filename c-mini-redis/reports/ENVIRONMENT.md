# 실측 환경 및 판정 규칙

- macOS 26.3 / Darwin 25.3.0, arm64, Apple M3, RAM 16 GiB.
- Apple clang 17.0.0 (clang-1700.6.4.2); 최적화 빌드 `-O2 -g -Wall -Wextra -Wpedantic`.
- Python 3.15.0b3. redis-benchmark 8.10.3, memtier_benchmark 2.6.0,
  Memcached 1.6.45, Lua 5.5.1, TShark 4.6.9.
- localhost TCP, 동시 연결 50, pipeline 깊이 16, 쓰기 1,000,000건.
- `tools/benchmark.py`는 요청 전체 256바이트를 assert한다. 값은 양쪽 모두
  191바이트, 키는 20바이트, TTL 300초이다. Redis는 RESP2 SET EX,
  Memcached는 native text set 및 헤더 공백 패딩을 사용한다.
- 비교 대상은 같은 호스트의 단일 워커 Memcached (`-t 1 -m 64`).
- 처리량은 연결 생성 후 barrier 해제부터 전체 응답 확인까지의 벽시계 시간으로
  계산한다. 클라이언트 처리 비용이 포함되므로 서버 단독 최대 처리량은 아니다.
- 합격: 성공 응답 1,000,000건, 오류 0건. 처리량 하한은 요구사항에 수치가 없어
  기준 미달 여부를 판정할 수 없다. JSON에 측정 시간과 초당 처리량을 기록한다.
- Wireshark 실캡처 시도: `tshark -i lo0 -f 'tcp port 6380' -a duration:2`.
  `/dev/bpf0: Permission denied`로 0패킷. 실제 패킷 캡처 검증은 미완료이다.
  시스템 권한 변경은 수행하지 않았다.

재현:

```sh
python3 tools/benchmark.py --protocol redis --port 6380
memcached -l 127.0.0.1 -p 11229 -t 1 -m 64
python3 tools/benchmark.py --protocol memcached --port 11229
```

각 JSON은 해당 프로토콜의 성공 응답을 직접 확인한 결과이다. 두 프로토콜의
인코딩·파싱 비용은 다르므로 결과를 순수 자료구조 성능 차이로 해석하지 않는다.

## 추가 도구 실행

```sh
redis-benchmark -h 127.0.0.1 -p 6380 -c 50 -n 100000 -P 16 -q SET bench smoke EX 300
memtier_benchmark --server=127.0.0.1 --port=6380 --protocol=redis --threads=1 --clients=50 --requests=2000 --pipeline=16 --command='SET __key__ __data__ EX 300' --data-size=191 --key-maximum=50 --hide-histogram
redis-check-rdb reports/benchmark-rdb/dump.rdb
```

redis-benchmark와 memtier 실행은 보조 검사다. 정확히 256바이트인 100만 건
본측정의 결과와 혼합하지 않는다. redis-benchmark는 미지원 CONFIG 조회 경고를
출력하지만 대상 SET 실행은 완료했다. 최종 스냅샷에서도 파일 형식과 CRC64 Checksum OK를 확인했다.
