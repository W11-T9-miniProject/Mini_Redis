#include "json_input.h"
#include <string.h>

static int utf8_valid(const unsigned char *data, size_t size) {
    for (size_t i = 0; i < size; ++i) {
        unsigned char c = data[i];
        if (!c) return 0;
        if (c < 0x80) continue;
        size_t bytes;
        if (c >= 0xc2 && c <= 0xdf) bytes = 2;
        else if (c >= 0xe0 && c <= 0xef) bytes = 3;
        else if (c >= 0xf0 && c <= 0xf4) bytes = 4;
        else return 0;
        if (bytes > size - i) return 0;
        for (size_t j = 1; j < bytes; ++j)
            if (data[i + j] < 0x80 || data[i + j] > 0xbf) return 0;
        unsigned char second = data[i + 1];
        if ((c == 0xe0 && second < 0xa0) || (c == 0xed && second > 0x9f) ||
            (c == 0xf0 && second < 0x90) || (c == 0xf4 && second > 0x8f)) return 0;
        i += bytes - 1;
    }
    return 1;
}

static int digit(char c) { return c >= '0' && c <= '9'; }

int json_input_valid(const char *body, size_t size) {
    if (!utf8_valid((const unsigned char *)body, size)) return 0;
    int in_string = 0, escaped = 0;
    for (size_t i = 0; i < size; ++i) {
        unsigned char c = (unsigned char)body[i];
        if (in_string) {
            if (c < 0x20) return 0;
            if (escaped) {
                if (c == 'u' && size - i >= 5 && memcmp(body + i, "u0000", 5) == 0)
                    return 0;
                escaped = 0;
            } else if (c == '\\') escaped = 1;
            else if (c == '"') in_string = 0;
        } else if (c == '"') in_string = 1;
        else if (c < 0x20 && c != '\t' && c != '\n' && c != '\r') return 0;
        else if (c == '-' || digit((char)c)) {
            size_t j = i;
            if (body[j] == '-') ++j;
            if (j == size || !digit(body[j])) return 0;
            if (body[j] == '0') {
                ++j;
                if (j < size && digit(body[j])) return 0;
            } else while (j < size && digit(body[j])) ++j;
            if (j < size && body[j] == '.') {
                ++j;
                if (j == size || !digit(body[j])) return 0;
                while (j < size && digit(body[j])) ++j;
            }
            if (j < size && (body[j] == 'e' || body[j] == 'E')) {
                ++j;
                if (j < size && (body[j] == '+' || body[j] == '-')) ++j;
                if (j == size || !digit(body[j])) return 0;
                while (j < size && digit(body[j])) ++j;
            }
            i = j - 1;
        }
    }
    return !in_string;
}
