#ifndef HASH_TABLE_H
#define HASH_TABLE_H

#include <stddef.h>
#include <stdint.h>
#include <pthread.h>

typedef enum {
    HT_OK,
    HT_NOT_FOUND,
    HT_NO_MEMORY,
    HT_INVALID_ARGUMENT,
    HT_CLOCK_ERROR,
    HT_CONDITION_NOT_MET,
    HT_LOCK_ERROR,
    HT_IO_ERROR,
    HT_CORRUPT_LOG
} ht_status;

typedef struct ht_entry {
    char *key;
    char *value;
    int64_t expires_at_ms; /* 0 = no expiration; otherwise Unix milliseconds. */
    struct ht_entry *next;
} ht_entry;

/* Fields may be read only when no operation is active. Never modify/copy them. */
typedef struct {
    ht_entry **buckets;
    size_t capacity;
    size_t size;
    pthread_mutex_t mutex;
    int initialized;
    struct ht_log *log;
    int replaying;
} hash_table;

/* Data operations share one mutex. Init/destroy require no concurrent callers.
 * Initialize once before use; destroy before reinitializing. */
ht_status ht_init(hash_table *table);
/* Open/recover an exclusively owned log before starting worker threads.
 * On failure the table is empty/uninitialized. Max key+value: 1 MiB per record. */
ht_status ht_open(hash_table *table, const char *log_path);
void ht_destroy(hash_table *table);
/* Ordinary set clears any previous expiration. */
ht_status ht_set(hash_table *table, const char *key, const char *value);
/* Unix milliseconds; 0 means no expiration. Past deadlines are allowed. */
ht_status ht_set_until(hash_table *table, const char *key, const char *value,
                       int64_t expires_at_ms);
/* ttl_ms must be positive; calculate a deadline using the server clock. */
ht_status ht_set_ttl(hash_table *table, const char *key, const char *value,
                     int64_t ttl_ms);
/* NX: ttl_ms = 0 means no expiration; positive values are relative TTL. */
ht_status ht_set_nx(hash_table *table, const char *key, const char *value,
                    int64_t ttl_ms);
/* On success, caller owns *value_out and must free it. Otherwise it is NULL.
 * Lookup may remove an expired node, so the table is mutable. */
ht_status ht_get(hash_table *table, const char *key, char **value_out);
ht_status ht_delete(hash_table *table, const char *key);
/* Missing/expired: HT_NOT_FOUND; different value: HT_CONDITION_NOT_MET. */
ht_status ht_compare_delete(hash_table *table, const char *key,
                             const char *expected_value);
ht_status ht_compare_set_until(hash_table *table, const char *key,
                                const char *expected_value, const char *value,
                                int64_t expires_at_ms);

#endif
