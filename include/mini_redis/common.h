#ifndef MINI_REDIS_COMMON_H
#define MINI_REDIS_COMMON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    MR_OK = 0,
    MR_NOT_FOUND,
    MR_NEED_MORE,    /* 오류 아님: 다음 recv()를 기다린다. */
    MR_INVALID,
    MR_OOM,
    MR_LIMIT,
    MR_IO
} MrStatus;

/* 입력/조회용 빌린 바이트열. 문자열 끝의 NUL에 의존하지 않는다. */
typedef struct {
    const unsigned char *data;
    size_t len;
} MrSlice;

/* 호출자가 소유하는 출력. 성공 후 mr_buffer_free()로 해제한다. */
typedef struct {
    unsigned char *data;
    size_t len;
} MrBuffer;

MrSlice mr_slice(const char *text); /* 예제/테스트 문자열용 편의 함수 */
bool mr_slice_equal(MrSlice a, MrSlice b);
void mr_buffer_free(MrBuffer *buffer);
const char *mr_status_name(MrStatus status);

/* 모든 코어 할당을 한곳으로 모으면 OOM을 재현할 수 있다.
 * 테스트에서만 n>=0을 사용: n번 성공 후 이후 할당 실패, -1이면 정상 복귀.
 * 서버에서는 설정하지 않는다. 서버 자체가 단일 스레드라는 전제다. */
void mr_alloc_fail_after(int64_t successful_allocations);
void *mr_malloc(size_t size);
void *mr_calloc(size_t count, size_t size);
void *mr_realloc(void *pointer, size_t size);

#endif
