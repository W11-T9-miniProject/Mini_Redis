#ifndef MINI_REDIS_LOG_H
#define MINI_REDIS_LOG_H

#include "hash_table.h"

typedef struct ht_log ht_log;
typedef ht_status (*log_replay_fn)(void *, int, const char *, const char *, int64_t);
enum { LOG_SET = 1, LOG_DELETE = 2 };
ht_status log_open(ht_log **out, const char *path, log_replay_fn replay, void *context);
ht_status log_append(ht_log *log, int operation, const char *key,
                     const char *value, int64_t deadline);
int log_failed(const ht_log *log);
void log_close(ht_log *log);

#endif
