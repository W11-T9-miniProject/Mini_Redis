#include "mini_redis/resp.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

/* offset에서 CRLF로 끝나는 unsigned 길이 필드를 읽는다.
 * 종료 바이트가 없으면 NEED_MORE, 완성된 문법 오류면 INVALID.
 * 상한을 넘는 숫자는 입력이 더 오기 전이라도 즉시 LIMIT로 거부한다. */
static MrStatus read_length(MrSlice input, size_t *offset, unsigned char marker,
                            size_t limit, size_t *number) {
    size_t at = *offset;
    if (at == input.len) return MR_NEED_MORE;
    if (input.data[at++] != marker) return MR_INVALID;
    size_t value = 0, digits = 0;
    while (at < input.len && input.data[at] != '\r') {
        unsigned char c = input.data[at++];
        if (c < '0' || c > '9') return MR_INVALID;
        size_t digit = (size_t)(c - '0');
        if (value > limit / 10 || (value == limit / 10 && digit > limit % 10)) return MR_LIMIT;
        value = value * 10 + digit;
        ++digits;
    }
    if (at == input.len) return MR_NEED_MORE;
    if (!digits) return MR_INVALID;
    if (input.len - at < 2) return MR_NEED_MORE;
    if (input.data[at + 1] != '\n') return MR_INVALID;
    *offset = at + 2;
    *number = value;
    return MR_OK;
}

MrStatus resp_parse(MrSlice input, RespCommand *command, size_t *consumed) {
    if (!command || !consumed) return MR_INVALID;
    memset(command, 0, sizeof(*command));
    *consumed = 0;
    if (input.len && !input.data) return MR_INVALID;
    size_t at = 0, argc = 0;
    MrStatus status = read_length(input, &at, '*', MR_MAX_ARGS, &argc);
    if (status != MR_OK) return status;
    if (!argc) return MR_INVALID;
    for (size_t i = 0; i < argc; ++i) {
        size_t length = 0;
        status = read_length(input, &at, '$', MR_MAX_FRAME_BYTES, &length);
        if (status != MR_OK) return status;
        /* 첫 프레임의 총 길이에만 제한 적용. 뒤의 정상 pipeline은 별개다.
         * limit-at-2를 먼저 확인해 size_t 덧셈 overflow도 피한다. */
        if (at > MR_MAX_FRAME_BYTES - 2 || length > MR_MAX_FRAME_BYTES - at - 2) return MR_LIMIT;
        if (input.len - at < length + 2) return MR_NEED_MORE;
        if (input.data[at + length] != '\r' || input.data[at + length + 1] != '\n') return MR_INVALID;
        command->args[i] = (MrSlice){input.data + at, length};
        at += length + 2;
    }
    command->argc = argc;
    *consumed = at;
    return MR_OK;
}

/* 출력 메모리의 소유권은 호출자에게 넘어간다.
 * 서버는 이 메모리를 출력 큐로 복사한 후 mr_buffer_free한다. */
static MrStatus text_reply(const char *prefix, MrSlice text, MrBuffer *out) {
    if (!out) return MR_INVALID;
    *out = (MrBuffer){NULL, 0};
    size_t prefix_len = strlen(prefix);
    if ((text.len && !text.data) || text.len > SIZE_MAX - prefix_len - 2) return MR_INVALID;
    for (size_t i = 0; i < text.len; ++i)
        if (text.data[i] == '\r' || text.data[i] == '\n') return MR_INVALID;
    out->len = prefix_len + text.len + 2;
    out->data = mr_malloc(out->len);
    if (!out->data) { out->len = 0; return MR_OOM; }
    memcpy(out->data, prefix, prefix_len);
    if (text.len) memcpy(out->data + prefix_len, text.data, text.len);
    memcpy(out->data + prefix_len + text.len, "\r\n", 2);
    return MR_OK;
}

MrStatus resp_simple(MrSlice text, MrBuffer *out) {
    return text_reply("+", text, out);
}

MrStatus resp_error(MrSlice text, MrBuffer *out) {
    return text_reply("-ERR ", text, out);
}

MrStatus resp_integer(int64_t number, MrBuffer *out) {
    char digits[32];
    int length = snprintf(digits, sizeof(digits), "%" PRId64, number);
    if (length < 0 || (size_t)length >= sizeof(digits)) return MR_INVALID;
    return text_reply(":", (MrSlice){(const unsigned char *)digits, (size_t)length}, out);
}

MrStatus resp_bulk(MrSlice value, MrBuffer *out) {
    if (!out) return MR_INVALID;
    *out = (MrBuffer){NULL, 0};
    if (value.len && !value.data) return MR_INVALID;
    char header[48];
    int length = snprintf(header, sizeof(header), "$%zu\r\n", value.len);
    if (length < 0 || (size_t)length >= sizeof(header) || value.len > SIZE_MAX - (size_t)length - 2)
        return MR_LIMIT;
    out->len = (size_t)length + value.len + 2;
    out->data = mr_malloc(out->len);
    if (!out->data) { out->len = 0; return MR_OOM; }
    memcpy(out->data, header, (size_t)length);
    if (value.len) memcpy(out->data + length, value.data, value.len);
    memcpy(out->data + length + value.len, "\r\n", 2);
    return MR_OK;
}

MrStatus resp_null(MrBuffer *out) {
    if (!out) return MR_INVALID;
    *out = (MrBuffer){NULL, 0};
    out->data = mr_malloc(5);
    if (!out->data) return MR_OOM;
    memcpy(out->data, "$-1\r\n", 5);
    out->len = 5;
    return MR_OK;
}
