#include "mini_redis/common.h"

#include <stdlib.h>
#include <string.h>

MrSlice mr_slice(const char *text) {
    MrSlice result = {(const unsigned char *)text, text ? strlen(text) : 0};
    return result;
}

bool mr_slice_equal(MrSlice a, MrSlice b) {
    return a.len == b.len && (a.len == 0 || memcmp(a.data, b.data, a.len) == 0);
}

void mr_buffer_free(MrBuffer *buffer) {
    if (!buffer) return;
    free(buffer->data);
    buffer->data = NULL;
    buffer->len = 0;
}

const char *mr_status_name(MrStatus status) {
    switch (status) {
        case MR_OK: return "OK";
        case MR_NOT_FOUND: return "NOT_FOUND";
        case MR_NEED_MORE: return "NEED_MORE";
        case MR_INVALID: return "INVALID";
        case MR_OOM: return "OUT_OF_MEMORY";
        case MR_LIMIT: return "LIMIT";
        case MR_IO: return "IO_ERROR";
    }
    return "UNKNOWN";
}

static int64_t allocations_left = -1;

void mr_alloc_fail_after(int64_t successful_allocations) {
    allocations_left = successful_allocations;
}

static bool allocation_allowed(void) {
    if (allocations_left == 0) return false;
    if (allocations_left > 0) --allocations_left;
    return true;
}

void *mr_malloc(size_t size) {
    return allocation_allowed() ? malloc(size ? size : 1) : NULL;
}

void *mr_calloc(size_t count, size_t size) {
    if (size && count > SIZE_MAX / size) return NULL;
    return allocation_allowed() ? calloc(count ? count : 1, size ? size : 1) : NULL;
}

void *mr_realloc(void *pointer, size_t size) {
    return allocation_allowed() ? realloc(pointer, size ? size : 1) : NULL;
}
