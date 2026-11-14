#ifndef MINI_REDIS_SERVER_H
#define MINI_REDIS_SERVER_H

#include "mini_redis/common.h"

#define MR_MAX_CLIENTS 128
#define MR_MAX_QUEUED_BYTES (4u * 1024u * 1024u)

/* 단일 이벤트 루프만 저장소에 접근한다. 클라이언트마다 버퍼를 분리한다.
 * intptr_t로 POSIX fd와 Windows SOCKET을 모두 보관한다. -1은 빈 슬롯. */
typedef struct {
    intptr_t fd;
    unsigned char *input;
    size_t input_len;
    size_t input_capacity;
    unsigned char *output;
    size_t output_len;
    size_t output_sent;
    size_t output_capacity;
    bool close_after_flush;
    bool read_closed;
    bool pending_commands;
} Client;

typedef struct {
    const char *bind_address;
    uint16_t port;
    size_t initial_capacity;
    size_t sweep_budget;
} ServerConfig;

/* 성공 시 종료할 때까지 실행. 실패/미구현은 0이 아닌 상태로 반환. */
MrStatus server_run(const ServerConfig *config);

#endif
