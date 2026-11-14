#include "hash_table.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>

static int use_test_clock;
static int clock_fails;
static int64_t test_now_ms;
int __real_clock_gettime(clockid_t clock_id, struct timespec *result);

int __wrap_clock_gettime(clockid_t clock_id, struct timespec *result) {
    if (!use_test_clock) return __real_clock_gettime(clock_id, result);
    assert(clock_id == CLOCK_REALTIME);
    if (clock_fails) return -1;
    result->tv_sec = (time_t)(test_now_ms / 1000);
    result->tv_nsec = (long)(test_now_ms % 1000) * 1000000L;
    return 0;
}

/* Fail exactly one allocation to check that existing data survives. */
static long allocations_until_failure = -1;
void *__real_malloc(size_t size);
void *__real_calloc(size_t count, size_t size);

static int should_fail(void) {
    if (allocations_until_failure < 0) return 0;
    if (allocations_until_failure-- == 0) return 1;
    return 0;
}

void *__wrap_malloc(size_t size) {
    return should_fail() ? NULL : __real_malloc(size);
}

void *__wrap_calloc(size_t count, size_t size) {
    return should_fail() ? NULL : __real_calloc(count, size);
}

static void expect_value(hash_table *table, const char *key,
                         const char *expected) {
    char *value = NULL;
    assert(ht_get(table, key, &value) == HT_OK);
    assert(value != NULL && strcmp(value, expected) == 0);
    free(value);
}

static void test_set_get_update(void) {
    hash_table table;
    assert(ht_init(&table) == HT_OK);
    assert(table.capacity == 16 && table.size == 0);
    assert(ht_set(&table, "seat:A1", "alice") == HT_OK);
    expect_value(&table, "seat:A1", "alice");
    assert(ht_set(&table, "seat:A1", "bob") == HT_OK);
    assert(table.size == 1);
    expect_value(&table, "seat:A1", "bob");
    ht_destroy(&table);
    assert(table.buckets == NULL && table.size == 0 && table.capacity == 0);
    ht_destroy(&table);
}

static void test_missing_and_empty(void) {
    hash_table table;
    char *value = NULL;
    assert(ht_init(&table) == HT_OK);
    assert(ht_get(&table, "missing", &value) == HT_NOT_FOUND && value == NULL);
    assert(ht_delete(&table, "missing") == HT_NOT_FOUND);
    assert(ht_set(&table, "", "") == HT_OK);
    expect_value(&table, "", "");
    assert(ht_delete(&table, "") == HT_OK);
    assert(ht_get(&table, "", &value) == HT_NOT_FOUND && value == NULL);
    ht_destroy(&table);
}

static void test_owned_copies(void) {
    hash_table table;
    char key[] = "seat:A1";
    char input[] = "alice";
    char *result = NULL;
    assert(ht_init(&table) == HT_OK);
    assert(ht_set(&table, key, input) == HT_OK);
    key[0] = 'X';
    input[0] = 'X';
    expect_value(&table, "seat:A1", "alice");
    assert(ht_get(&table, "seat:A1", &result) == HT_OK);
    assert(ht_set(&table, "seat:A1", "bob") == HT_OK);
    assert(strcmp(result, "alice") == 0);
    assert(ht_delete(&table, "seat:A1") == HT_OK);
    ht_destroy(&table);
    assert(strcmp(result, "alice") == 0);
    free(result);
}

static void test_collision_deletions(void) {
    /* These three keys collide at capacity 16 with FNV-1a. */
    const char *keys[] = {"a", "q", "A"};
    for (size_t target = 0; target < 3; ++target) {
        hash_table table;
        assert(ht_init(&table) == HT_OK);
        for (size_t i = 0; i < 3; ++i)
            assert(ht_set(&table, keys[i], keys[i]) == HT_OK);
        size_t occupied = 0;
        ht_entry *head = NULL;
        for (size_t i = 0; i < table.capacity; ++i) {
            if (table.buckets[i]) { ++occupied; head = table.buckets[i]; }
        }
        assert(occupied == 1 && head && head->next && head->next->next);
        ht_entry *victim = head;
        for (size_t i = 0; i < target; ++i) victim = victim->next;
        char removed[2];
        strcpy(removed, victim->key);
        assert(ht_delete(&table, removed) == HT_OK);
        assert(table.size == 2);
        char *value = NULL;
        assert(ht_get(&table, removed, &value) == HT_NOT_FOUND && value == NULL);
        for (size_t i = 0; i < 3; ++i)
            if (strcmp(keys[i], removed) != 0)
                expect_value(&table, keys[i], keys[i]);
        ht_destroy(&table);
    }
}

static void test_growth(void) {
    hash_table table;
    char key[32];
    assert(ht_init(&table) == HT_OK);
    for (size_t i = 0; i < 16; ++i) {
        snprintf(key, sizeof(key), "key:%zu", i);
        assert(ht_set(&table, key, key) == HT_OK);
    }
    assert(table.capacity == 16 && table.size == 16);
    assert(ht_set(&table, "key:0", "updated") == HT_OK);
    assert(table.capacity == 16 && table.size == 16);
    assert(ht_set(&table, "key:16", "key:16") == HT_OK);
    assert(table.capacity == 32 && table.size == 17);
    for (size_t i = 17; i < 1000; ++i) {
        snprintf(key, sizeof(key), "key:%zu", i);
        assert(ht_set(&table, key, key) == HT_OK);
    }
    assert(table.capacity == 1024 && table.size == 1000);
    for (size_t i = 0; i < 1000; ++i) {
        snprintf(key, sizeof(key), "key:%zu", i);
        expect_value(&table, key, i == 0 ? "updated" : key);
    }
    for (size_t i = 0; i < 1000; ++i) {
        snprintf(key, sizeof(key), "key:%zu", i);
        assert(ht_delete(&table, key) == HT_OK);
    }
    assert(table.size == 0);
    ht_destroy(&table);
}

static void test_invalid_arguments(void) {
    hash_table table;
    char *value = NULL;
    assert(ht_init(NULL) == HT_INVALID_ARGUMENT);
    assert(ht_init(&table) == HT_OK);
    assert(ht_set(NULL, "a", "b") == HT_INVALID_ARGUMENT);
    assert(ht_set(&table, NULL, "b") == HT_INVALID_ARGUMENT);
    assert(ht_set(&table, "a", NULL) == HT_INVALID_ARGUMENT);
    assert(ht_get(&table, "a", NULL) == HT_INVALID_ARGUMENT);
    assert(ht_get(NULL, "a", &value) == HT_INVALID_ARGUMENT && value == NULL);
    assert(ht_get(&table, NULL, &value) == HT_INVALID_ARGUMENT && value == NULL);
    assert(ht_delete(NULL, "a") == HT_INVALID_ARGUMENT);
    assert(ht_delete(&table, NULL) == HT_INVALID_ARGUMENT);
    ht_destroy(&table);
    assert(ht_set(&table, "a", "b") == HT_INVALID_ARGUMENT);
    ht_destroy(NULL);
}

static void test_allocation_failures(void) {
    hash_table table;
    allocations_until_failure = 0;
    assert(ht_init(&table) == HT_NO_MEMORY);
    assert(table.buckets == NULL && table.size == 0 && table.capacity == 0);
    ht_destroy(&table);
    assert(ht_init(&table) == HT_OK);
    assert(ht_set(&table, "existing", "original") == HT_OK);
    allocations_until_failure = 0;
    assert(ht_set(&table, "existing", "replacement") == HT_NO_MEMORY);
    expect_value(&table, "existing", "original");
    for (long fail = 0; fail < 3; ++fail) {
        allocations_until_failure = fail;
        assert(ht_set(&table, "new", "value") == HT_NO_MEMORY);
        assert(table.size == 1);
        char *value = NULL;
        assert(ht_get(&table, "new", &value) == HT_NOT_FOUND && value == NULL);
    }
    char *result = NULL;
    allocations_until_failure = 0;
    assert(ht_get(&table, "existing", &result) == HT_NO_MEMORY && result == NULL);
    expect_value(&table, "existing", "original");
    ht_destroy(&table);

    for (long fail = 0; fail < 4; ++fail) {
        assert(ht_init(&table) == HT_OK);
        char key[16];
        for (int i = 0; i < 16; ++i) {
            snprintf(key, sizeof(key), "k%d", i);
            assert(ht_set(&table, key, key) == HT_OK);
        }
        allocations_until_failure = fail;
        assert(ht_set(&table, "overflow", "value") == HT_NO_MEMORY);
        assert(table.size == 16 && table.capacity == 16);
        for (int i = 0; i < 16; ++i) {
            snprintf(key, sizeof(key), "k%d", i);
            expect_value(&table, key, key);
        }
        ht_destroy(&table);
    }
}

static void test_expiration_boundary(void) {
    hash_table table;
    char *value = NULL;
    use_test_clock = 1;
    test_now_ms = 100000;
    assert(ht_init(&table) == HT_OK);
    assert(ht_set_ttl(&table, "seat:A1", "alice", 20000) == HT_OK);
    test_now_ms = 119999;
    expect_value(&table, "seat:A1", "alice");
    test_now_ms = 120000;
    /* Time passing alone has not physically removed the node. */
    assert(table.size == 1);
    assert(ht_get(&table, "seat:A1", &value) == HT_NOT_FOUND && value == NULL);
    assert(table.size == 0);
    assert(ht_get(&table, "seat:A1", &value) == HT_NOT_FOUND);
    assert(ht_set_until(&table, "past", "value", 119999) == HT_OK);
    assert(ht_get(&table, "past", &value) == HT_NOT_FOUND && value == NULL);
    assert(ht_set_until(&table, "later", "value", 130000) == HT_OK);
    test_now_ms = 130001;
    assert(ht_delete(&table, "later") == HT_NOT_FOUND && table.size == 0);
    ht_destroy(&table);
    use_test_clock = 0;
}

static void test_expiration_update_and_failure(void) {
    hash_table table;
    char *value = NULL;
    use_test_clock = 1;
    test_now_ms = 100000;
    assert(ht_init(&table) == HT_OK);
    assert(ht_set_until(&table, "seat", "first", 110000) == HT_OK);
    allocations_until_failure = 0;
    assert(ht_set_until(&table, "seat", "failed", 150000) == HT_NO_MEMORY);
    expect_value(&table, "seat", "first");
    test_now_ms = 110000;
    assert(ht_get(&table, "seat", &value) == HT_NOT_FOUND);
    assert(ht_set_ttl(&table, "seat", "second", 20000) == HT_OK);
    assert(table.size == 1);
    test_now_ms = 111000;
    assert(ht_set(&table, "seat", "permanent") == HT_OK);
    test_now_ms = 999999;
    expect_value(&table, "seat", "permanent");
    assert(ht_set_until(&table, "seat", "third", 1000000) == HT_OK);
    test_now_ms = 1000000;
    assert(ht_set_ttl(&table, "seat", "fourth", 20000) == HT_OK);
    assert(table.size == 1);
    expect_value(&table, "seat", "fourth");
    test_now_ms = 1020000;
    assert(ht_get(&table, "seat", &value) == HT_NOT_FOUND);
    ht_destroy(&table);
    use_test_clock = 0;
}

static void test_expiration_collision_and_growth(void) {
    const char *keys[] = {"a", "q", "A"};
    use_test_clock = 1;
    test_now_ms = 100000;
    for (size_t target = 0; target < 3; ++target) {
        hash_table table;
        assert(ht_init(&table) == HT_OK);
        for (size_t i = 0; i < 3; ++i)
            assert(ht_set_until(&table, keys[i], keys[i],
                                i == target ? 100000 : 200000) == HT_OK);
        char *value = NULL;
        assert(ht_get(&table, keys[target], &value) == HT_NOT_FOUND);
        assert(table.size == 2);
        for (size_t i = 0; i < 3; ++i)
            if (i != target) expect_value(&table, keys[i], keys[i]);
        ht_destroy(&table);
    }
    hash_table table;
    assert(ht_init(&table) == HT_OK);
    char key[16];
    for (int i = 0; i < 17; ++i) {
        snprintf(key, sizeof(key), "key:%d", i);
        assert(ht_set_until(&table, key, key, 200000) == HT_OK);
    }
    assert(table.capacity == 32);
    test_now_ms = 199999;
    expect_value(&table, "key:0", "key:0");
    test_now_ms = 200000;
    for (int i = 0; i < 17; ++i) {
        char *value = NULL;
        snprintf(key, sizeof(key), "key:%d", i);
        assert(ht_get(&table, key, &value) == HT_NOT_FOUND);
    }
    assert(table.size == 0);
    ht_destroy(&table);
    use_test_clock = 0;
}

static void test_expiration_invalid_and_clock_failure(void) {
    hash_table table;
    char *value = NULL;
    use_test_clock = 1;
    test_now_ms = 100000;
    assert(ht_init(&table) == HT_OK);
    assert(ht_set_ttl(&table, "key", "value", 0) == HT_INVALID_ARGUMENT);
    assert(ht_set_ttl(&table, "key", "value", -1) == HT_INVALID_ARGUMENT);
    assert(ht_set_ttl(&table, "key", "value", INT64_MAX) == HT_INVALID_ARGUMENT);
    assert(ht_set_until(&table, "key", "value", -1) == HT_INVALID_ARGUMENT);
    assert(ht_set_ttl(NULL, "key", "value", 20) == HT_INVALID_ARGUMENT);
    assert(ht_set_until(&table, NULL, "value", 20) == HT_INVALID_ARGUMENT);
    assert(ht_set_until(&table, "key", NULL, 20) == HT_INVALID_ARGUMENT);
    assert(table.size == 0);
    assert(ht_set_until(&table, "key", "original", 120000) == HT_OK);
    clock_fails = 1;
    assert(ht_get(&table, "key", &value) == HT_CLOCK_ERROR && value == NULL);
    assert(ht_delete(&table, "key") == HT_CLOCK_ERROR);
    assert(ht_set_until(&table, "key", "changed", 130000) == HT_CLOCK_ERROR);
    assert(ht_set_ttl(&table, "other", "value", 20) == HT_CLOCK_ERROR);
    assert(table.size == 1);
    clock_fails = 0;
    expect_value(&table, "key", "original");
    ht_destroy(&table);
    use_test_clock = 0;
}

static void test_nx_conditions(void) {
    hash_table table;
    use_test_clock = 1;
    test_now_ms = 100000;
    assert(ht_init(&table) == HT_OK);
    assert(ht_set_nx(&table, "seat", "alice", 20000) == HT_OK);
    assert(ht_set_nx(&table, "seat", "bob", 90000) == HT_CONDITION_NOT_MET);
    expect_value(&table, "seat", "alice");
    assert(table.size == 1);
    test_now_ms = 120000;
    assert(ht_set_nx(&table, "seat", "bob", 20000) == HT_OK);
    expect_value(&table, "seat", "bob");
    assert(table.size == 1);
    assert(ht_set_nx(&table, "permanent", "value", 0) == HT_OK);
    assert(ht_set_nx(&table, "permanent", "other", 0) == HT_CONDITION_NOT_MET);
    assert(ht_set_nx(&table, "bad", "value", -1) == HT_INVALID_ARGUMENT);
    assert(ht_set_nx(NULL, "bad", "value", 0) == HT_INVALID_ARGUMENT);
    assert(ht_set_nx(&table, NULL, "value", 0) == HT_INVALID_ARGUMENT);
    assert(ht_set_nx(&table, "bad", NULL, 0) == HT_INVALID_ARGUMENT);
    assert(ht_set_nx(&table, "bad", "value", INT64_MAX) == HT_INVALID_ARGUMENT);
    allocations_until_failure = 0;
    assert(ht_set_nx(&table, "no-memory", "value", 0) == HT_NO_MEMORY);
    assert(ht_set_nx(&table, "no-memory", "retry", 0) == HT_OK);
    clock_fails = 1;
    assert(ht_set_nx(&table, "clock-error", "value", 20) == HT_CLOCK_ERROR);
    clock_fails = 0;
    expect_value(&table, "no-memory", "retry");
    ht_destroy(&table);
    use_test_clock = 0;
}

typedef struct {
    hash_table *table;
    pthread_barrier_t *start;
    int id;
    ht_status result;
} thread_job;

static void wait_at_barrier(pthread_barrier_t *barrier) {
    int result = pthread_barrier_wait(barrier);
    assert(result == 0 || result == PTHREAD_BARRIER_SERIAL_THREAD);
}

static void *claim_seat(void *argument) {
    thread_job *job = argument;
    char value[32];
    snprintf(value, sizeof(value), "claim:%d", job->id);
    wait_at_barrier(job->start);
    job->result = ht_set_nx(job->table, "seat:A1", value, 60000);
    return NULL;
}

static void test_concurrent_nx(void) {
    enum { THREADS = 100 };
    hash_table table;
    pthread_t threads[THREADS];
    thread_job jobs[THREADS];
    pthread_barrier_t start;
    use_test_clock = 1;
    test_now_ms = 100000;
    assert(ht_init(&table) == HT_OK);
    assert(pthread_barrier_init(&start, NULL, THREADS + 1) == 0);
    for (int i = 0; i < THREADS; ++i) {
        jobs[i] = (thread_job){&table, &start, i, HT_INVALID_ARGUMENT};
        assert(pthread_create(&threads[i], NULL, claim_seat, &jobs[i]) == 0);
    }
    wait_at_barrier(&start);
    int successes = 0;
    int winner = -1;
    for (int i = 0; i < THREADS; ++i) {
        assert(pthread_join(threads[i], NULL) == 0);
        if (jobs[i].result == HT_OK) { ++successes; winner = i; }
        else assert(jobs[i].result == HT_CONDITION_NOT_MET);
    }
    assert(successes == 1 && table.size == 1);
    char expected[32];
    snprintf(expected, sizeof(expected), "claim:%d", winner);
    expect_value(&table, "seat:A1", expected);
    assert(pthread_barrier_destroy(&start) == 0);
    ht_destroy(&table);
    use_test_clock = 0;
}

static void *mixed_operations(void *argument) {
    thread_job *job = argument;
    wait_at_barrier(job->start);
    for (int i = 0; i < 200; ++i) {
        char key[32];
        snprintf(key, sizeof(key), "worker:%d:key:%d", job->id, i);
        assert(ht_set(job->table, key, key) == HT_OK);
        expect_value(job->table, key, key);
        if (i % 2 == 0) assert(ht_delete(job->table, key) == HT_OK);
        /* Contend on the same key while other threads copy/delete its value. */
        assert(ht_set(job->table, "shared", "value") == HT_OK);
        char *value = NULL;
        ht_status status = ht_get(job->table, "shared", &value);
        assert(status == HT_OK || status == HT_NOT_FOUND);
        if (status == HT_OK) assert(strcmp(value, "value") == 0);
        free(value);
        status = ht_delete(job->table, "shared");
        assert(status == HT_OK || status == HT_NOT_FOUND);
    }
    return NULL;
}

static void test_concurrent_mixed_operations(void) {
    enum { THREADS = 8 };
    hash_table table;
    pthread_t threads[THREADS];
    thread_job jobs[THREADS];
    pthread_barrier_t start;
    assert(ht_init(&table) == HT_OK);
    assert(pthread_barrier_init(&start, NULL, THREADS + 1) == 0);
    for (int i = 0; i < THREADS; ++i) {
        jobs[i] = (thread_job){&table, &start, i, HT_OK};
        assert(pthread_create(&threads[i], NULL, mixed_operations, &jobs[i]) == 0);
    }
    wait_at_barrier(&start);
    for (int i = 0; i < THREADS; ++i) assert(pthread_join(threads[i], NULL) == 0);
    assert(table.size == 800 && table.capacity == 1024);
    for (int worker = 0; worker < THREADS; ++worker) {
        for (int i = 0; i < 200; ++i) {
            char key[32];
            snprintf(key, sizeof(key), "worker:%d:key:%d", worker, i);
            if (i % 2) expect_value(&table, key, key);
            else {
                char *value = NULL;
                assert(ht_get(&table, key, &value) == HT_NOT_FOUND);
            }
        }
    }
    assert(pthread_barrier_destroy(&start) == 0);
    ht_destroy(&table);
}

static void test_conditional_delete(void) {
    hash_table table;
    char *value = NULL;
    assert(ht_init(&table) == HT_OK);
    assert(ht_set(&table, "seat", "hold:bbb") == HT_OK);
    assert(ht_compare_delete(&table, "seat", "hold:aaa") == HT_CONDITION_NOT_MET);
    expect_value(&table, "seat", "hold:bbb");
    assert(ht_compare_delete(&table, "seat", "hold:bbb") == HT_OK);
    assert(ht_compare_delete(&table, "seat", "hold:bbb") == HT_NOT_FOUND);
    assert(ht_get(&table, "seat", &value) == HT_NOT_FOUND);
    assert(ht_set(&table, "", "") == HT_OK);
    assert(ht_compare_delete(&table, "", "") == HT_OK);
    const char *keys[] = {"a", "q", "A"};
    for (size_t target = 0; target < 3; ++target) {
        for (size_t i = 0; i < 3; ++i)
            assert(ht_set(&table, keys[i], keys[i]) == HT_OK);
        assert(ht_compare_delete(&table, keys[target], keys[target]) == HT_OK);
        for (size_t i = 0; i < 3; ++i) {
            if (i != target) {
                expect_value(&table, keys[i], keys[i]);
                assert(ht_delete(&table, keys[i]) == HT_OK);
            }
        }
    }
    assert(table.size == 0);
    ht_destroy(&table);
}

static void test_conditional_update_and_expiration(void) {
    hash_table table;
    char *value = NULL;
    use_test_clock = 1;
    test_now_ms = 100000;
    assert(ht_init(&table) == HT_OK);
    assert(ht_set_nx(&table, "seat", "hold:aaa", 20000) == HT_OK);
    assert(ht_compare_set_until(&table, "seat", "wrong", "bad", 999999)
           == HT_CONDITION_NOT_MET);
    expect_value(&table, "seat", "hold:aaa");
    assert(ht_compare_set_until(&table, "seat", "hold:aaa", "confirmed:aaa", 200000)
           == HT_OK);
    assert(table.size == 1);
    test_now_ms = 120000;
    expect_value(&table, "seat", "confirmed:aaa");
    assert(ht_compare_delete(&table, "seat", "hold:aaa") == HT_CONDITION_NOT_MET);
    test_now_ms = 200000;
    assert(ht_compare_set_until(&table, "seat", "confirmed:aaa", "late", 300000)
           == HT_NOT_FOUND);
    assert(table.size == 0);
    assert(ht_set_nx(&table, "seat", "hold:bbb", 20000) == HT_OK);
    assert(ht_compare_delete(&table, "seat", "hold:aaa") == HT_CONDITION_NOT_MET);
    assert(ht_compare_set_until(&table, "seat", "hold:aaa", "stolen", 0)
           == HT_CONDITION_NOT_MET);
    expect_value(&table, "seat", "hold:bbb");
    test_now_ms = 220000;
    assert(ht_compare_delete(&table, "seat", "hold:bbb") == HT_NOT_FOUND);
    assert(ht_get(&table, "seat", &value) == HT_NOT_FOUND);
    assert(ht_compare_set_until(&table, "missing", "", "new", 0) == HT_NOT_FOUND);
    assert(ht_set(&table, "key", "old") == HT_OK);
    assert(ht_compare_set_until(&table, "key", "old", "new", 0) == HT_OK);
    test_now_ms = 999999;
    expect_value(&table, "key", "new");
    assert(ht_compare_set_until(&table, "key", "new", "past", 1) == HT_OK);
    assert(ht_get(&table, "key", &value) == HT_NOT_FOUND);
    ht_destroy(&table);
    use_test_clock = 0;
}

static void test_conditional_errors_preserve_state(void) {
    hash_table table;
    char *value = NULL;
    use_test_clock = 1;
    test_now_ms = 100000;
    assert(ht_init(&table) == HT_OK);
    assert(ht_set_until(&table, "seat", "original", 120000) == HT_OK);
    assert(ht_compare_delete(NULL, "seat", "original") == HT_INVALID_ARGUMENT);
    assert(ht_compare_delete(&table, NULL, "original") == HT_INVALID_ARGUMENT);
    assert(ht_compare_delete(&table, "seat", NULL) == HT_INVALID_ARGUMENT);
    assert(ht_compare_set_until(NULL, "seat", "original", "new", 0) == HT_INVALID_ARGUMENT);
    assert(ht_compare_set_until(&table, NULL, "original", "new", 0) == HT_INVALID_ARGUMENT);
    assert(ht_compare_set_until(&table, "seat", NULL, "new", 0) == HT_INVALID_ARGUMENT);
    assert(ht_compare_set_until(&table, "seat", "original", NULL, 0) == HT_INVALID_ARGUMENT);
    assert(ht_compare_set_until(&table, "seat", "original", "new", -1) == HT_INVALID_ARGUMENT);
    allocations_until_failure = 0;
    assert(ht_compare_set_until(&table, "seat", "original", "new", 300000) == HT_NO_MEMORY);
    expect_value(&table, "seat", "original");
    clock_fails = 1;
    assert(ht_compare_delete(&table, "seat", "original") == HT_CLOCK_ERROR);
    assert(ht_compare_set_until(&table, "seat", "original", "new", 300000) == HT_CLOCK_ERROR);
    clock_fails = 0;
    expect_value(&table, "seat", "original");
    test_now_ms = 120000;
    assert(ht_get(&table, "seat", &value) == HT_NOT_FOUND);
    ht_destroy(&table);
    assert(ht_compare_delete(&table, "seat", "original") == HT_INVALID_ARGUMENT);
    assert(ht_compare_set_until(&table, "seat", "original", "new", 0) == HT_INVALID_ARGUMENT);
    use_test_clock = 0;
}

static void *confirm_seat(void *argument) {
    thread_job *job = argument;
    char value[32];
    snprintf(value, sizeof(value), "confirmed:%d", job->id);
    wait_at_barrier(job->start);
    job->result = ht_compare_set_until(job->table, "seat:A1", "hold", value, 0);
    return NULL;
}

static void test_concurrent_conditional_update(void) {
    enum { THREADS = 100 };
    hash_table table;
    pthread_t threads[THREADS];
    thread_job jobs[THREADS];
    pthread_barrier_t start;
    assert(ht_init(&table) == HT_OK);
    assert(ht_set(&table, "seat:A1", "hold") == HT_OK);
    assert(pthread_barrier_init(&start, NULL, THREADS + 1) == 0);
    for (int i = 0; i < THREADS; ++i) {
        jobs[i] = (thread_job){&table, &start, i, HT_INVALID_ARGUMENT};
        assert(pthread_create(&threads[i], NULL, confirm_seat, &jobs[i]) == 0);
    }
    wait_at_barrier(&start);
    int successes = 0;
    int winner = -1;
    for (int i = 0; i < THREADS; ++i) {
        assert(pthread_join(threads[i], NULL) == 0);
        if (jobs[i].result == HT_OK) { ++successes; winner = i; }
        else assert(jobs[i].result == HT_CONDITION_NOT_MET);
    }
    assert(successes == 1 && table.size == 1);
    char expected[32];
    snprintf(expected, sizeof(expected), "confirmed:%d", winner);
    expect_value(&table, "seat:A1", expected);
    assert(pthread_barrier_destroy(&start) == 0);
    ht_destroy(&table);
}

static void *cancel_seat(void *argument) {
    thread_job *job = argument;
    wait_at_barrier(job->start);
    job->result = ht_compare_delete(job->table, "seat:A1", "hold");
    return NULL;
}

static void test_concurrent_cancel_and_confirm(void) {
    hash_table table;
    pthread_t cancel_thread, confirm_thread;
    pthread_barrier_t start;
    assert(ht_init(&table) == HT_OK);
    assert(ht_set(&table, "seat:A1", "hold") == HT_OK);
    assert(pthread_barrier_init(&start, NULL, 3) == 0);
    thread_job cancel = {&table, &start, 0, HT_INVALID_ARGUMENT};
    thread_job confirm = {&table, &start, 1, HT_INVALID_ARGUMENT};
    assert(pthread_create(&cancel_thread, NULL, cancel_seat, &cancel) == 0);
    assert(pthread_create(&confirm_thread, NULL, confirm_seat, &confirm) == 0);
    wait_at_barrier(&start);
    assert(pthread_join(cancel_thread, NULL) == 0);
    assert(pthread_join(confirm_thread, NULL) == 0);
    if (cancel.result == HT_OK) {
        assert(confirm.result == HT_NOT_FOUND);
        char *value = NULL;
        assert(ht_get(&table, "seat:A1", &value) == HT_NOT_FOUND);
    } else {
        assert(cancel.result == HT_CONDITION_NOT_MET && confirm.result == HT_OK);
        expect_value(&table, "seat:A1", "confirmed:1");
    }
    assert(pthread_barrier_destroy(&start) == 0);
    ht_destroy(&table);
}

int main(void) {
    test_set_get_update(); puts("PASS set/get/update");
    test_missing_and_empty(); puts("PASS missing/empty/delete");
    test_owned_copies(); puts("PASS owned input/output copies");
    test_collision_deletions(); puts("PASS collision head/middle/tail deletion");
    test_growth(); puts("PASS growth and 1000-key round trip");
    test_invalid_arguments(); puts("PASS invalid arguments");
    test_allocation_failures(); puts("PASS allocation failure preserves data");
    test_expiration_boundary(); puts("PASS expiration boundary and lazy removal");
    test_expiration_update_and_failure(); puts("PASS expiration update and allocation failure");
    test_expiration_collision_and_growth(); puts("PASS expiration collision and growth");
    test_expiration_invalid_and_clock_failure(); puts("PASS expiration validation and clock errors");
    test_nx_conditions(); puts("PASS NX conditions, expiration and errors");
    test_concurrent_nx(); puts("PASS 100 concurrent NX requests: exactly one winner");
    test_concurrent_mixed_operations(); puts("PASS concurrent set/get/delete and growth");
    test_conditional_delete(); puts("PASS conditional delete and collision chains");
    test_conditional_update_and_expiration(); puts("PASS conditional update, stale IDs and expiration");
    test_conditional_errors_preserve_state(); puts("PASS conditional errors preserve value and deadline");
    test_concurrent_conditional_update(); puts("PASS 100 concurrent conditional updates: one winner");
    test_concurrent_cancel_and_confirm(); puts("PASS concurrent cancellation vs confirmation");
    return 0;
}
