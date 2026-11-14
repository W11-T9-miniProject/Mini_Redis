CC = gcc
CPPFLAGS = -D_POSIX_C_SOURCE=200809L -Iinclude
CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -Werror -g -pthread
SOURCES = src/hash_table.c src/log.c tests/test_hash_table.c
WRAP_ALLOC = -Wl,--wrap=malloc -Wl,--wrap=calloc -Wl,--wrap=clock_gettime
WRAP_LOG = $(WRAP_ALLOC) -Wl,--wrap=write -Wl,--wrap=fsync
SERVER_SOURCES = src/server.c src/json_input.c src/hash_table.c src/log.c
HTTP_CFLAGS = $(shell pkg-config --cflags libmicrohttpd libcjson)
HTTP_LIBS = $(shell pkg-config --libs libmicrohttpd libcjson) -lm

.PHONY: test sanitize clean server http-test http-sanitize web-test web menu-test

web-test: build/mini-redis-server
	python3 -m unittest tests/test_web.py -v

menu-test: build/mini-redis-server
	python3 -m unittest tests.test_menu.MenuTests -v

web:
	python3 -m web.app

server: build/mini-redis-server

build/mini-redis-server: $(SERVER_SOURCES) include/hash_table.h src/log.h src/json_input.h Makefile | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(HTTP_CFLAGS) $(SERVER_SOURCES) $(HTTP_LIBS) -o $@

build/mini-redis-server-sanitize: $(SERVER_SOURCES) include/hash_table.h src/log.h src/json_input.h Makefile | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(HTTP_CFLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer $(SERVER_SOURCES) $(HTTP_LIBS) -o $@

http-test: build/mini-redis-server
	python3 tests/test_http.py

http-sanitize: build/mini-redis-server-sanitize
	MINI_REDIS_SERVER=build/mini-redis-server-sanitize ASAN_OPTIONS=detect_leaks=1 python3 tests/test_http.py

test: build/test_hash_table build/test_persistence
	./build/test_hash_table
	./build/test_persistence

build/test_hash_table: $(SOURCES) include/hash_table.h src/log.h Makefile | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SOURCES) $(WRAP_ALLOC) -o $@

sanitize: build/test_hash_table_sanitize build/test_persistence_sanitize
	ASAN_OPTIONS=detect_leaks=1 ./build/test_hash_table_sanitize
	ASAN_OPTIONS=detect_leaks=1 ./build/test_persistence_sanitize

build/test_persistence: src/hash_table.c src/log.c src/log.h tests/test_persistence.c include/hash_table.h Makefile | build
	$(CC) $(CPPFLAGS) $(CFLAGS) src/hash_table.c src/log.c tests/test_persistence.c $(WRAP_LOG) -o $@

build/test_persistence_sanitize: src/hash_table.c src/log.c src/log.h tests/test_persistence.c include/hash_table.h Makefile | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer src/hash_table.c src/log.c tests/test_persistence.c $(WRAP_LOG) -o $@

build/test_hash_table_sanitize: $(SOURCES) include/hash_table.h src/log.h Makefile | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer $(SOURCES) $(WRAP_ALLOC) -o $@

build:
	mkdir -p build

clean:
	rm -f build/test_hash_table build/test_hash_table_sanitize
	rm -f build/test_persistence build/test_persistence_sanitize
	rm -f build/mini-redis-server build/mini-redis-server-sanitize
