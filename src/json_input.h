#ifndef MINI_REDIS_JSON_INPUT_H
#define MINI_REDIS_JSON_INPUT_H
#include <stddef.h>
/* Check lexical cases cJSON accepts permissively; cJSON checks the structure. */
int json_input_valid(const char *body, size_t size);
#endif
