CC = cc
CPPFLAGS = -Iinclude
CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -g
LDFLAGS =

CORE = src/common.c src/hash_table.c src/store.c src/resp.c src/command.c
HEADERS = $(wildcard include/mini_redis/*.h)
TESTS = tests/test_main.c tests/test_hash_table.c tests/test_store.c tests/test_resp.c tests/test_command.c

.PHONY: all test test-hash test-store test-resp test-command sanitize clean
all: build/mini-redis build/unit-tests

build:
	mkdir -p build

build/mini-redis: $(CORE) src/server.c src/main.c $(HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(CORE) src/server.c src/main.c $(LDFLAGS) -o $@

build/unit-tests: $(CORE) $(TESTS) tests/test.h $(HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(CORE) $(TESTS) $(LDFLAGS) -o $@

# 각 단계의 계약을 실제로 검증한다. 하나라도 실패하면 0이 아닌 코드로 종료한다.
test: build/unit-tests
	./build/unit-tests all
test-hash: build/unit-tests
	./build/unit-tests hash
test-store: build/unit-tests
	./build/unit-tests store
test-resp: build/unit-tests
	./build/unit-tests resp
test-command: build/unit-tests
	./build/unit-tests command

# 별도 바이너리: 기존 빌드 옵션/산출물을 섞지 않는다.
sanitize: | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer $(CORE) $(TESTS) -fsanitize=address,undefined -o build/unit-tests-sanitize
	ASAN_OPTIONS=detect_leaks=1 ./build/unit-tests-sanitize all

clean:
	rm -rf build
