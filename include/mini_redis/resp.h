#ifndef MINI_REDIS_RESP_H
#define MINI_REDIS_RESP_H

#include "mini_redis/common.h"

#define MR_MAX_ARGS 8
#define MR_MAX_FRAME_BYTES (1024u * 1024u)

/* 요청은 RESP2의 'bulk string 배열'만 지원한다. 중첩 배열/null 인자 제외.
 * args는 입력 버퍼를 참조한다. 입력을 compact/realloc하기 전에 실행을 끝낸다. */
typedef struct {
    size_t argc;
    MrSlice args[MR_MAX_ARGS];
} RespCommand;

/* 완성: MR_OK, consumed>0. 미완성: MR_NEED_MORE, consumed=0.
 * 잘못된 형식: MR_INVALID. 크기 상한: MR_LIMIT. 파서는 저장소를 변경하지 않는다.
 * 입력에 여러 명령이 있으면 첫 명령만 파싱하고 consumed로 경계를 알려준다. */
MrStatus resp_parse(MrSlice input, RespCommand *command, size_t *consumed);

MrStatus resp_simple(MrSlice text, MrBuffer *out);
MrStatus resp_error(MrSlice text, MrBuffer *out);
MrStatus resp_integer(int64_t number, MrBuffer *out);
MrStatus resp_bulk(MrSlice value, MrBuffer *out);
MrStatus resp_null(MrBuffer *out);

#endif
