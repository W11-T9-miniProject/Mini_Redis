#include "hash_table.h"
#include "log.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define INITIAL_CAPACITY 16


static ht_status lock_table(hash_table *table) {
    /* initialized stays unchanged until all callers stop and destroy runs. */
    if (!table || !table->initialized) return HT_INVALID_ARGUMENT;
    return pthread_mutex_lock(&table->mutex) == 0 ? HT_OK : HT_LOCK_ERROR;
}

static ht_status finish_operation(hash_table *table, ht_status status) {
    return pthread_mutex_unlock(&table->mutex) == 0 ? status : HT_LOCK_ERROR;
}

static uint64_t hash_key(const char *key) {
    uint64_t hash = UINT64_C(14695981039346656037);
    const unsigned char *byte = (const unsigned char *)key;
    while (*byte) {
        hash ^= *byte++;
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static char *copy_string(const char *source) {
    size_t length = strlen(source) + 1;
    char *copy = malloc(length);
    if (copy) memcpy(copy, source, length);
    return copy;
}

static ht_status replace_value(hash_table *table, ht_entry *entry, const char *value,
                                int64_t expires_at_ms) {
    /* Allocate first so failure preserves both the original value and deadline. */
    char *replacement = copy_string(value);
    if (!replacement) return HT_NO_MEMORY;
    ht_status status = log_append(table->log, LOG_SET, entry->key, value, expires_at_ms);
    if (status != HT_OK) { free(replacement); return status; }
    free(entry->value);
    entry->value = replacement;
    entry->expires_at_ms = expires_at_ms;
    return HT_OK;
}

static ht_status current_time_ms(int64_t *now_ms) {
    struct timespec now;
    if (clock_gettime(CLOCK_REALTIME, &now) != 0 || now.tv_sec < 0 ||
        now.tv_sec > (INT64_MAX - 999) / 1000)
        return HT_CLOCK_ERROR;
    *now_ms = (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
    return HT_OK;
}

static void free_entry(ht_entry *entry) {
    free(entry->key);
    free(entry->value);
    free(entry);
}

static void unlink_entry(hash_table *table, size_t index,
                         ht_entry *previous, ht_entry *entry) {
    if (previous) previous->next = entry->next;
    else table->buckets[index] = entry->next;
    free_entry(entry);
    --table->size;
}

/* Shared by reads, writes and deletes so an expired key is always absent. */
static ht_status find_entry(hash_table *table, const char *key,
                             ht_entry **found) {
    *found = NULL;
    size_t index = hash_key(key) % table->capacity;
    ht_entry *previous = NULL;
    for (ht_entry *entry = table->buckets[index]; entry; entry = entry->next) {
        if (strcmp(entry->key, key) == 0) {
            if (!table->replaying && entry->expires_at_ms != 0) {
                int64_t now_ms;
                ht_status status = current_time_ms(&now_ms);
                if (status != HT_OK) return status;
                if (now_ms >= entry->expires_at_ms) {
                    unlink_entry(table, index, previous, entry);
                    return HT_NOT_FOUND;
                }
            }
            *found = entry;
            return HT_OK;
        }
        previous = entry;
    }
    return HT_NOT_FOUND;
}

static ht_status grow(hash_table *table) {
    if (table->capacity > SIZE_MAX / 2 / sizeof(*table->buckets))
        return HT_NO_MEMORY;

    size_t capacity = table->capacity * 2;
    ht_entry **buckets = calloc(capacity, sizeof(*buckets));
    if (!buckets) return HT_NO_MEMORY;

    for (size_t i = 0; i < table->capacity; ++i) {
        ht_entry *entry = table->buckets[i];
        while (entry) {
            /* Save the old next before reconnecting this node in the new array. */
            ht_entry *next = entry->next;
            size_t index = hash_key(entry->key) % capacity;
            entry->next = buckets[index];
            buckets[index] = entry;
            entry = next;
        }
    }
    free(table->buckets);
    table->buckets = buckets;
    table->capacity = capacity;
    return HT_OK;
}

ht_status ht_init(hash_table *table) {
    if (!table) return HT_INVALID_ARGUMENT;
    *table = (hash_table){0};
    table->buckets = calloc(INITIAL_CAPACITY, sizeof(*table->buckets));
    if (!table->buckets) return HT_NO_MEMORY;
    if (pthread_mutex_init(&table->mutex, NULL) != 0) {
        free(table->buckets);
        *table = (hash_table){0};
        return HT_LOCK_ERROR;
    }
    table->capacity = INITIAL_CAPACITY;
    table->initialized = 1;
    return HT_OK;
}

void ht_destroy(hash_table *table) {
    if (!table) return;
    for (size_t i = 0; i < table->capacity; ++i) {
        ht_entry *entry = table->buckets[i];
        while (entry) {
            ht_entry *next = entry->next;
            free_entry(entry);
            entry = next;
        }
    }
    free(table->buckets);
    if (table->initialized) pthread_mutex_destroy(&table->mutex);
    log_close(table->log);
    *table = (hash_table){0};
}

ht_status ht_set(hash_table *table, const char *key, const char *value) {
    return ht_set_until(table, key, value, 0);
}

/* Only call while holding table->mutex; never call a public operation here. */
static ht_status set_until_locked(hash_table *table, const char *key,
                                  const char *value, int64_t expires_at_ms,
                                  int nx) {
    if (!table || !table->buckets || !key || !value || expires_at_ms < 0)
        return HT_INVALID_ARGUMENT;
    if (log_failed(table->log)) return HT_IO_ERROR;

    ht_entry *existing;
    ht_status status = find_entry(table, key, &existing);
    if (status != HT_OK && status != HT_NOT_FOUND) return status;
    if (existing) {
        if (nx) return HT_CONDITION_NOT_MET;
        return replace_value(table, existing, value, expires_at_ms);
    }

    ht_entry *entry = malloc(sizeof(*entry));
    if (!entry) return HT_NO_MEMORY;
    entry->key = copy_string(key);
    if (!entry->key) { free(entry); return HT_NO_MEMORY; }
    entry->value = copy_string(value);
    if (!entry->value) { free(entry->key); free(entry); return HT_NO_MEMORY; }
    entry->expires_at_ms = expires_at_ms;

    if (table->size >= table->capacity) {
        ht_status status = grow(table);
        if (status != HT_OK) { free_entry(entry); return status; }
    }
    status = log_append(table->log, LOG_SET, key, value, expires_at_ms);
    if (status != HT_OK) { free_entry(entry); return status; }
    size_t index = hash_key(key) % table->capacity;
    entry->next = table->buckets[index];
    table->buckets[index] = entry;
    ++table->size;
    return HT_OK;
}

static ht_status get_locked(hash_table *table, const char *key, char **value_out) {
    if (!value_out) return HT_INVALID_ARGUMENT;
    *value_out = NULL;
    if (!table || !table->buckets || !key) return HT_INVALID_ARGUMENT;

    ht_entry *entry;
    ht_status status = find_entry(table, key, &entry);
    if (status != HT_OK) return status;
    *value_out = copy_string(entry->value);
    return *value_out ? HT_OK : HT_NO_MEMORY;
}

static ht_status delete_entry_locked(hash_table *table, ht_entry *found) {
    ht_status status = log_append(table->log, LOG_DELETE, found->key, NULL, 0);
    if (status != HT_OK) return status;
    size_t index = hash_key(found->key) % table->capacity;
    ht_entry *previous = NULL;
    ht_entry *entry = table->buckets[index];

    while (entry) {
        if (entry == found) {
            unlink_entry(table, index, previous, entry);
            return HT_OK;
        }
        previous = entry;
        entry = entry->next;
    }
    return HT_NOT_FOUND;
}

static ht_status delete_locked(hash_table *table, const char *key) {
    if (log_failed(table->log)) return HT_IO_ERROR;
    ht_entry *found;
    ht_status status = find_entry(table, key, &found);
    if (status != HT_OK) return status;
    return delete_entry_locked(table, found);
}

static ht_status set_relative_locked(hash_table *table, const char *key,
                                     const char *value, int64_t ttl_ms, int nx) {
    if (log_failed(table->log)) return HT_IO_ERROR;
    int64_t deadline = 0;
    if (ttl_ms > 0) {
        int64_t now_ms;
        ht_status status = current_time_ms(&now_ms);
        if (status != HT_OK) return status;
        if (ttl_ms > INT64_MAX - now_ms) return HT_INVALID_ARGUMENT;
        deadline = now_ms + ttl_ms;
    }
    return set_until_locked(table, key, value, deadline, nx);
}

ht_status ht_set_until(hash_table *table, const char *key, const char *value,
                       int64_t expires_at_ms) {
    if (!key || !value || expires_at_ms < 0) return HT_INVALID_ARGUMENT;
    ht_status status = lock_table(table);
    if (status != HT_OK) return status;
    status = set_until_locked(table, key, value, expires_at_ms, 0);
    return finish_operation(table, status);
}

ht_status ht_set_ttl(hash_table *table, const char *key, const char *value,
                     int64_t ttl_ms) {
    if (!key || !value || ttl_ms <= 0) return HT_INVALID_ARGUMENT;
    ht_status status = lock_table(table);
    if (status != HT_OK) return status;
    status = set_relative_locked(table, key, value, ttl_ms, 0);
    return finish_operation(table, status);
}

ht_status ht_set_nx(hash_table *table, const char *key, const char *value,
                    int64_t ttl_ms) {
    if (!key || !value || ttl_ms < 0) return HT_INVALID_ARGUMENT;
    ht_status status = lock_table(table);
    if (status != HT_OK) return status;
    status = set_relative_locked(table, key, value, ttl_ms, 1);
    return finish_operation(table, status);
}

ht_status ht_get(hash_table *table, const char *key, char **value_out) {
    if (!value_out) return HT_INVALID_ARGUMENT;
    *value_out = NULL;
    if (!key) return HT_INVALID_ARGUMENT;
    ht_status status = lock_table(table);
    if (status != HT_OK) return status;
    status = get_locked(table, key, value_out);
    return finish_operation(table, status);
}

ht_status ht_delete(hash_table *table, const char *key) {
    if (!key) return HT_INVALID_ARGUMENT;
    ht_status status = lock_table(table);
    if (status != HT_OK) return status;
    status = delete_locked(table, key);
    return finish_operation(table, status);
}

ht_status ht_compare_delete(hash_table *table, const char *key,
                             const char *expected_value) {
    if (!key || !expected_value) return HT_INVALID_ARGUMENT;
    ht_status status = lock_table(table);
    if (status != HT_OK) return status;
    ht_entry *entry;
    status = log_failed(table->log) ? HT_IO_ERROR : find_entry(table, key, &entry);
    if (status == HT_OK) {
        status = strcmp(entry->value, expected_value) == 0
                 ? delete_entry_locked(table, entry) : HT_CONDITION_NOT_MET;
    }
    return finish_operation(table, status);
}

ht_status ht_compare_set_until(hash_table *table, const char *key,
                                const char *expected_value, const char *value,
                                int64_t expires_at_ms) {
    if (!key || !expected_value || !value || expires_at_ms < 0)
        return HT_INVALID_ARGUMENT;
    ht_status status = lock_table(table);
    if (status != HT_OK) return status;
    ht_entry *entry;
    status = log_failed(table->log) ? HT_IO_ERROR : find_entry(table, key, &entry);
    if (status == HT_OK) {
        status = strcmp(entry->value, expected_value) == 0
                 ? replace_value(table, entry, value, expires_at_ms)
                 : HT_CONDITION_NOT_MET;
    }
    return finish_operation(table, status);
}

static ht_status replay_change(void *context, int operation, const char *key,
                                const char *value, int64_t deadline) {
    hash_table *table = context;
    if (operation == LOG_SET) return set_until_locked(table, key, value, deadline, 0);
    ht_status status = delete_locked(table, key);
    return status == HT_NOT_FOUND ? HT_OK : status;
}

ht_status ht_open(hash_table *table, const char *log_path) {
    if (!table) return HT_INVALID_ARGUMENT;
    if (!log_path || !*log_path) { *table = (hash_table){0}; return HT_INVALID_ARGUMENT; }
    ht_status status = ht_init(table);
    if (status != HT_OK) return status;
    table->replaying = 1;
    status = log_open(&table->log, log_path, replay_change, table);
    table->replaying = 0;
    if (status == HT_OK) {
        int64_t now_ms;
        status = current_time_ms(&now_ms);
        if (status == HT_OK) {
            for (size_t i = 0; i < table->capacity; ++i) {
                ht_entry *previous = NULL;
                ht_entry *entry = table->buckets[i];
                while (entry) {
                    ht_entry *next = entry->next;
                    if (entry->expires_at_ms && now_ms >= entry->expires_at_ms)
                        unlink_entry(table, i, previous, entry);
                    else previous = entry;
                    entry = next;
                }
            }
        }
    }
    if (status != HT_OK) ht_destroy(table);
    return status;
}
