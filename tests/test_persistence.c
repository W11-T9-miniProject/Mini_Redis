#include "hash_table.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int64_t now_ms = 100000;
static long write_budget = -1;
static int fail_sync;
static long allocation_budget = -1;
void *__real_malloc(size_t size);
void *__real_calloc(size_t count, size_t size);
ssize_t __real_write(int fd, const void *buffer, size_t size);
int __real_fsync(int fd);

static int fail_allocation(void) {
    if (allocation_budget < 0) return 0;
    return allocation_budget-- == 0;
}

void *__wrap_malloc(size_t size) {
    return fail_allocation() ? NULL : __real_malloc(size);
}

void *__wrap_calloc(size_t count, size_t size) {
    return fail_allocation() ? NULL : __real_calloc(count, size);
}

ssize_t __wrap_write(int fd, const void *buffer, size_t size) {
    if (write_budget == 0) { errno = EIO; return -1; }
    if (write_budget > 0 && size > (size_t)write_budget) size = (size_t)write_budget;
    ssize_t written = __real_write(fd, buffer, size);
    if (written > 0 && write_budget > 0) write_budget -= written;
    return written;
}

int __wrap_fsync(int fd) {
    if (fail_sync) { fail_sync = 0; errno = EIO; return -1; }
    return __real_fsync(fd);
}

int __wrap_clock_gettime(clockid_t id, struct timespec *value) {
    assert(id == CLOCK_REALTIME);
    value->tv_sec = (time_t)(now_ms / 1000);
    value->tv_nsec = (long)(now_ms % 1000) * 1000000L;
    return 0;
}

static void expect_value(hash_table *table, const char *key, const char *expected) {
    char *value = NULL;
    assert(ht_get(table, key, &value) == HT_OK);
    assert(strcmp(value, expected) == 0);
    free(value);
}

static void make_path(char *path) {
    strcpy(path, "/tmp/mini-redis-log-XXXXXX");
    int fd = mkstemp(path);
    assert(fd >= 0 && close(fd) == 0);
}

static off_t file_size(const char *path) {
    struct stat info;
    assert(stat(path, &info) == 0);
    return info.st_size;
}

static void test_recovery(void) {
    char path[64];
    hash_table table;
    make_path(path);
    assert(ht_open(&table, path) == HT_OK);
    assert(ht_set(&table, "", "") == HT_OK);
    assert(ht_set_nx(&table, "seat", "hold", 20000) == HT_OK);
    assert(ht_compare_set_until(&table, "seat", "hold", "confirmed", 200000) == HT_OK);
    assert(ht_set(&table, "delete-me", "value") == HT_OK);
    assert(ht_delete(&table, "delete-me") == HT_OK);
    assert(ht_set(&table, "cancel", "hold") == HT_OK);
    assert(ht_compare_delete(&table, "cancel", "hold") == HT_OK);
    off_t size = file_size(path);
    assert(ht_set_nx(&table, "seat", "other", 20) == HT_CONDITION_NOT_MET);
    assert(ht_compare_delete(&table, "seat", "wrong") == HT_CONDITION_NOT_MET);
    assert(ht_compare_set_until(&table, "seat", "wrong", "bad", 0) == HT_CONDITION_NOT_MET);
    assert(file_size(path) == size);
    ht_destroy(&table);
    now_ms = 120000;
    assert(ht_open(&table, path) == HT_OK);
    expect_value(&table, "seat", "confirmed");
    expect_value(&table, "", "");
    char *value = NULL;
    assert(ht_get(&table, "delete-me", &value) == HT_NOT_FOUND);
    assert(ht_get(&table, "cancel", &value) == HT_NOT_FOUND);
    assert(file_size(path) == size); /* Replay must not append the same records. */
    ht_destroy(&table);
    now_ms = 200000;
    assert(ht_open(&table, path) == HT_OK);
    assert(ht_get(&table, "seat", &value) == HT_NOT_FOUND);
    assert(table.size == 1);
    ht_destroy(&table);
    assert(unlink(path) == 0);
    now_ms = 100000;
}

static void test_incomplete_tail(void) {
    char path[64];
    hash_table table;
    make_path(path);
    assert(ht_open(&table, path) == HT_OK);
    assert(ht_set(&table, "keep", "value") == HT_OK);
    off_t good = file_size(path);
    assert(ht_set(&table, "lost", "incomplete-payload") == HT_OK);
    ht_destroy(&table);
    assert(truncate(path, file_size(path) - 3) == 0);
    assert(ht_open(&table, path) == HT_OK);
    expect_value(&table, "keep", "value");
    char *value = NULL;
    assert(ht_get(&table, "lost", &value) == HT_NOT_FOUND);
    assert(file_size(path) == good);
    assert(ht_set(&table, "after", "repaired") == HT_OK);
    ht_destroy(&table);
    int fd = open(path, O_WRONLY | O_APPEND);
    assert(fd >= 0);
    assert(__real_write(fd, "partial", 7) == 7);
    assert(close(fd) == 0);
    assert(ht_open(&table, path) == HT_OK);
    expect_value(&table, "after", "repaired");
    ht_destroy(&table);
    assert(unlink(path) == 0);
}

static void test_corruption_and_locking(void) {
    char path[64];
    hash_table table, second;
    make_path(path);
    assert(ht_open(&table, path) == HT_OK);
    assert(ht_open(&second, path) == HT_IO_ERROR);
    assert(ht_set(&table, "first", "value") == HT_OK);
    assert(ht_set(&table, "second", "value") == HT_OK);
    ht_destroy(&table);
    off_t size = file_size(path);
    int fd = open(path, O_RDWR);
    char byte;
    assert(fd >= 0 && pread(fd, &byte, 1, 32) == 1);
    byte ^= 1;
    assert(pwrite(fd, &byte, 1, 32) == 1 && close(fd) == 0);
    assert(ht_open(&table, path) == HT_CORRUPT_LOG);
    assert(!table.initialized && file_size(path) == size);
    ht_destroy(&table);
    assert(unlink(path) == 0);
}

static void test_write_failure(void) {
    char path[64];
    hash_table table;
    make_path(path);
    assert(ht_open(&table, path) == HT_OK);
    assert(ht_set(&table, "seat", "original") == HT_OK);
    write_budget = 9; /* Partial header followed by an I/O failure. */
    assert(ht_set(&table, "seat", "uncertain") == HT_IO_ERROR);
    write_budget = -1;
    expect_value(&table, "seat", "original");
    assert(ht_set(&table, "other", "value") == HT_IO_ERROR);
    assert(ht_set_nx(&table, "other", "value", 0) == HT_IO_ERROR);
    assert(ht_delete(&table, "seat") == HT_IO_ERROR);
    assert(ht_compare_delete(&table, "seat", "original") == HT_IO_ERROR);
    assert(ht_compare_set_until(&table, "seat", "original", "new", 0) == HT_IO_ERROR);
    ht_destroy(&table);
    assert(ht_open(&table, path) == HT_OK);
    expect_value(&table, "seat", "original");
    assert(ht_set(&table, "seat", "recovered") == HT_OK);
    ht_destroy(&table);
    assert(unlink(path) == 0);
}

static void test_corrupt_length_is_not_truncated(void) {
    char path[64];
    hash_table table;
    make_path(path);
    assert(ht_open(&table, path) == HT_OK);
    assert(ht_set(&table, "first", "value") == HT_OK);
    assert(ht_set(&table, "second", "value") == HT_OK);
    ht_destroy(&table);
    off_t size = file_size(path);
    int fd = open(path, O_RDWR);
    /* A plausible length bigger than the remaining file must not erase history. */
    unsigned char length = 133;
    assert(fd >= 0 && pwrite(fd, &length, 1, 8) == 1);
    unsigned char total = 170; /* 32-byte header + 133 key bytes + 5 value bytes. */
    assert(pwrite(fd, &total, 1, 24) == 1 && close(fd) == 0);
    assert(ht_open(&table, path) == HT_CORRUPT_LOG);
    assert(file_size(path) == size);
    assert(unlink(path) == 0);
}

static void test_sync_failure(void) {
    char path[64];
    hash_table table;
    make_path(path);
    assert(ht_open(&table, path) == HT_OK);
    assert(ht_set(&table, "seat", "original") == HT_OK);
    fail_sync = 1;
    assert(ht_set(&table, "seat", "uncertain") == HT_IO_ERROR);
    expect_value(&table, "seat", "original");
    assert(ht_set(&table, "other", "value") == HT_IO_ERROR);
    ht_destroy(&table);
    /* A full write with a failed fsync is an uncertain outcome, not rollback. */
    assert(ht_open(&table, path) == HT_OK);
    expect_value(&table, "seat", "uncertain");
    ht_destroy(&table);
    assert(unlink(path) == 0);
}

static void test_killed_process(void) {
    char path[64];
    int ready[2];
    make_path(path);
    assert(pipe(ready) == 0);
    pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        close(ready[0]);
        hash_table table;
        assert(ht_open(&table, path) == HT_OK);
        assert(ht_set(&table, "seat", "confirmed") == HT_OK);
        assert(write(ready[1], "!", 1) == 1);
        for (;;) pause();
    }
    close(ready[1]);
    char signal;
    assert(read(ready[0], &signal, 1) == 1);
    assert(close(ready[0]) == 0 && kill(child, SIGKILL) == 0);
    int status;
    assert(waitpid(child, &status, 0) == child && WIFSIGNALED(status));
    hash_table table;
    assert(ht_open(&table, path) == HT_OK);
    expect_value(&table, "seat", "confirmed");
    ht_destroy(&table);
    assert(unlink(path) == 0);
}

static void test_preparation_failure_and_limits(void) {
    char path[64];
    hash_table table;
    make_path(path);
    assert(unlink(path) == 0); /* Exercise actual file creation as well. */
    assert(ht_open(&table, path) == HT_OK);
    assert(ht_set(&table, "seat", "original") == HT_OK);
    off_t size = file_size(path);
    allocation_budget = 0;
    assert(ht_set(&table, "seat", "replacement") == HT_NO_MEMORY);
    expect_value(&table, "seat", "original");
    assert(file_size(path) == size);
    char *large = malloc(1024 * 1024 + 1);
    assert(large != NULL);
    memset(large, 'x', 1024 * 1024);
    large[1024 * 1024] = 0;
    assert(ht_set(&table, "seat", large) == HT_INVALID_ARGUMENT);
    assert(file_size(path) == size);
    expect_value(&table, "seat", "original");
    free(large);
    assert(ht_set(&table, "special\nkey", "quotes\"\tand\nlines") == HT_OK);
    ht_destroy(&table);
    /* Fail the first payload allocation during replay; do not damage the log. */
    size = file_size(path);
    allocation_budget = 2;
    assert(ht_open(&table, path) == HT_NO_MEMORY);
    assert(!table.initialized && file_size(path) == size);
    assert(ht_open(&table, path) == HT_OK);
    expect_value(&table, "seat", "original");
    expect_value(&table, "special\nkey", "quotes\"\tand\nlines");
    ht_destroy(&table);
    assert(unlink(path) == 0);
    assert(ht_open(NULL, path) == HT_INVALID_ARGUMENT);
    assert(ht_open(&table, NULL) == HT_INVALID_ARGUMENT && !table.initialized);
}

typedef struct { hash_table *table; int id; } log_thread_job;

static void *write_log_records(void *argument) {
    log_thread_job *job = argument;
    for (int i = 0; i < 20; ++i) {
        char key[32];
        snprintf(key, sizeof(key), "worker:%d:%d", job->id, i);
        assert(ht_set(job->table, key, key) == HT_OK);
    }
    return NULL;
}

static void test_concurrent_log_replay(void) {
    char path[64];
    hash_table table;
    pthread_t threads[4];
    log_thread_job jobs[4];
    make_path(path);
    assert(ht_open(&table, path) == HT_OK);
    for (int i = 0; i < 4; ++i) {
        jobs[i] = (log_thread_job){&table, i};
        assert(pthread_create(&threads[i], NULL, write_log_records, &jobs[i]) == 0);
    }
    for (int i = 0; i < 4; ++i) assert(pthread_join(threads[i], NULL) == 0);
    ht_destroy(&table);
    assert(ht_open(&table, path) == HT_OK);
    assert(table.size == 80 && table.capacity == 128);
    for (int worker = 0; worker < 4; ++worker) {
        for (int i = 0; i < 20; ++i) {
            char key[32];
            snprintf(key, sizeof(key), "worker:%d:%d", worker, i);
            expect_value(&table, key, key);
        }
    }
    ht_destroy(&table);
    assert(unlink(path) == 0);
}

int main(void) {
    test_recovery(); puts("PASS log replay, all mutations and absolute expiration");
    test_incomplete_tail(); puts("PASS incomplete tail repair and subsequent append");
    test_corruption_and_locking(); puts("PASS corruption rejection and exclusive log ownership");
    test_corrupt_length_is_not_truncated(); puts("PASS corrupted lengths preserve log history");
    test_write_failure(); puts("PASS partial write failure blocks mutations, keeps reads");
    test_sync_failure(); puts("PASS fsync failure and uncertain recovery outcome");
    test_killed_process(); puts("PASS confirmed write survives SIGKILL");
    test_preparation_failure_and_limits(); puts("PASS persistent allocation failures, creation and limits");
    test_concurrent_log_replay(); puts("PASS concurrent durable writes and rehash replay");
    return 0;
}
