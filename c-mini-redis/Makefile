CC ?= cc
CFLAGS ?= -O2 -g -Wall -Wextra -Wpedantic
LUA_PREFIX ?= /opt/homebrew/opt/lua
CPPFLAGS += -I$(LUA_PREFIX)/include/lua -I$(LUA_PREFIX)/include
LDLIBS += -L$(LUA_PREFIX)/lib -llua -lm
all: mini-redis
SOURCES := $(wildcard src/*.c)
mini-redis: $(SOURCES) src/server.h src/ae.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SOURCES) -o $@ $(LDLIBS)
mini-redis-asan: $(SOURCES) src/server.h src/ae.h
	$(CC) $(CPPFLAGS) -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-omit-frame-pointer $(SOURCES) -o $@ $(LDLIBS)
asan: mini-redis-asan
clean:
	rm -f mini-redis mini-redis-asan
.PHONY: all clean asan
