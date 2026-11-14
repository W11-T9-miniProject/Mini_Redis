#include "mini_redis/command.h"

/* locale에 따라 달라지는 문자열 함수 대신 ASCII 명령 이름만 비교한다.
 * 키/값은 이 변환을 거치지 않으므로 대소문자를 구분한다. */
static bool word(MrSlice input, const char *expected) {
    MrSlice other = mr_slice(expected);
    if (input.len != other.len || (input.len && !input.data)) return false;
    for (size_t i = 0; i < input.len; ++i) {
        unsigned char c = input.data[i];
        if (c >= 'a' && c <= 'z') c = (unsigned char)(c - 'a' + 'A');
        if (c != other.data[i]) return false;
    }
    return true;
}

/* 부호 없는 크기로 누적하면 INT64_MIN도 안전하게 처리할 수 있다.
 * 공백, '+', 숫자 뒤의 쓰레기 문자를 모두 거부한다. */
static bool integer(MrSlice input, int64_t *out) {
    if (!input.len || !input.data) return false;
    bool negative = input.data[0] == '-';
    size_t start = negative ? 1 : 0;
    if (start == input.len) return false;
    uint64_t limit = negative ? (uint64_t)INT64_MAX + 1 : (uint64_t)INT64_MAX;
    uint64_t value = 0;
    for (size_t i = start; i < input.len; ++i) {
        unsigned char c = input.data[i];
        if (c < '0' || c > '9') return false;
        uint64_t digit = (uint64_t)(c - '0');
        if (value > (limit - digit) / 10) return false;
        value = value * 10 + digit;
    }
    *out = negative ? (value == (uint64_t)INT64_MAX + 1 ? INT64_MIN : -(int64_t)value) : (int64_t)value;
    return true;
}

static MrStatus error(MrBuffer *reply, const char *message) {
    return resp_error(mr_slice(message), reply);
}

MrStatus command_execute(Store *store, const RespCommand *command,
                         int64_t now_ms, MrBuffer *reply) {
    if (!reply) return MR_INVALID;
    *reply = (MrBuffer){NULL, 0};
    if (!store || !command || !command->argc || command->argc > MR_MAX_ARGS || now_ms < 0)
        return MR_INVALID;
    for (size_t i = 0; i < command->argc; ++i)
        if (command->args[i].len && !command->args[i].data) return MR_INVALID;
    MrSlice name = command->args[0];
    if (word(name, "PING")) {
        if (command->argc != 1) return error(reply, "wrong number of arguments");
        return resp_simple(mr_slice("PONG"), reply);
    }
    if (word(name, "SET")) {
        int64_t ttl = -1;
        if (command->argc != 3 && command->argc != 5) return error(reply, "wrong number of arguments");
        if (command->argc == 5 && (!word(command->args[3], "PX") ||
            !integer(command->args[4], &ttl) || ttl <= 0)) return error(reply, "invalid PX milliseconds");
        if (ttl > 0 && now_ms > INT64_MAX - ttl) return error(reply, "TTL overflow");
        /* 응답 메모리까지 먼저 확보한다. OOM이면 SET을 시작하지 않는다.
         * 값/TTL의 교체는 store_set 한 번에서 일어난다. */
        MrStatus status = resp_simple(mr_slice("OK"), reply);
        if (status != MR_OK) return status;
        status = store_set(store, command->args[1], command->args[2], ttl, now_ms);
        if (status != MR_OK) mr_buffer_free(reply);
        return status;
    }
    if (word(name, "GET")) {
        if (command->argc != 2) return error(reply, "wrong number of arguments");
        MrSlice value = {0};
        MrStatus status = store_get(store, command->args[1], now_ms, &value);
        if (status == MR_NOT_FOUND) return resp_null(reply);
        /* store_get 결과는 빌린 참조다. 바로 응답 메모리에 복사해야 다음 삭제에 안전. */
        return status == MR_OK ? resp_bulk(value, reply) : status;
    }
    if (word(name, "DEL") || word(name, "EXPIRE")) {
        bool is_expire = word(name, "EXPIRE");
        if (command->argc != (is_expire ? 3u : 2u)) return error(reply, "wrong number of arguments");
        int64_t seconds = 0, ttl_ms = 0;
        if (is_expire) {
            if (!integer(command->args[2], &seconds)) return error(reply, "invalid seconds");
            if (seconds > INT64_MAX / 1000) return error(reply, "TTL overflow");
            /* 음수는 모두 즉시 삭제이므로 곱하지 않아 INT64_MIN 곱셈도 피한다. */
            ttl_ms = seconds <= 0 ? 0 : seconds * 1000;
            if (ttl_ms > 0 && now_ms > INT64_MAX - ttl_ms) return error(reply, "TTL overflow");
        }
        MrStatus status = resp_integer(0, reply);
        if (status != MR_OK) return status;
        status = is_expire ? store_expire(store, command->args[1], ttl_ms, now_ms)
                           : store_delete(store, command->args[1], now_ms);
        if (status == MR_OK) reply->data[1] = '1'; /* :0과 :1은 길이가 같다. */
        else if (status != MR_NOT_FOUND) { mr_buffer_free(reply); return status; }
        return MR_OK;
    }
    if (word(name, "TTL")) {
        if (command->argc != 2) return error(reply, "wrong number of arguments");
        int64_t remaining = -2;
        MrStatus status = store_ttl(store, command->args[1], now_ms, &remaining);
        if (status != MR_OK && status != MR_NOT_FOUND) return status;
        return resp_integer(remaining < 0 ? remaining : remaining / 1000, reply);
    }
    return error(reply, "unknown command");
}
