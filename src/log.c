#include "log.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

enum { HEADER_SIZE = 32, MAX_PAYLOAD = 1024 * 1024 };

struct ht_log { int fd; int failed; };

static void encode(unsigned char *out, uint64_t value, size_t count) {
    for (size_t i = 0; i < count; ++i) { out[i] = (unsigned char)value; value >>= 8; }
}

static uint64_t decode(const unsigned char *in, size_t count) {
    uint64_t value = 0;
    for (size_t i = 0; i < count; ++i) value |= (uint64_t)in[i] << (8 * i);
    return value;
}

static uint32_t checksum(uint32_t hash, const void *bytes, size_t size) {
    const unsigned char *data = bytes;
    for (size_t i = 0; i < size; ++i) { hash ^= data[i]; hash *= UINT32_C(16777619); }
    return hash;
}

static int sync_file(int fd) {
    int result;
    do { result = fsync(fd); } while (result < 0 && errno == EINTR);
    return result;
}

static int write_all(int fd, const void *bytes, size_t size) {
    const unsigned char *data = bytes;
    while (size) {
        ssize_t written = write(fd, data, size);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) return -1;
        data += written;
        size -= (size_t)written;
    }
    return 0;
}

/* Return the number read, including a short read at EOF, or -1 on error. */
static ssize_t read_record(int fd, void *bytes, size_t size) {
    size_t total = 0;
    while (total < size) {
        ssize_t got = read(fd, (unsigned char *)bytes + total, size - total);
        if (got < 0 && errno == EINTR) continue;
        if (got < 0) return -1;
        if (got == 0) break;
        total += (size_t)got;
    }
    return (ssize_t)total;
}

int log_failed(const ht_log *log) { return log && log->failed; }

ht_status log_append(ht_log *log, int operation, const char *key,
                     const char *value, int64_t deadline) {
    if (!log) return HT_OK;
    if (log->failed) return HT_IO_ERROR;
    size_t key_size = strlen(key), value_size = value ? strlen(value) : 0;
    if (key_size > MAX_PAYLOAD || value_size > MAX_PAYLOAD - key_size)
        return HT_INVALID_ARGUMENT;
    unsigned char header[HEADER_SIZE] = {0};
    memcpy(header, "MRD1", 4);
    header[4] = (unsigned char)operation;
    encode(header + 8, key_size, 4);
    encode(header + 12, value_size, 4);
    encode(header + 16, (uint64_t)deadline, 8);
    /* Validate lengths before trusting them to classify a tail as incomplete. */
    encode(header + 24, checksum(UINT32_C(2166136261), header, 24), 4);
    uint32_t hash = checksum(UINT32_C(2166136261), key, key_size);
    hash = checksum(hash, value, value_size);
    encode(header + 28, hash, 4);
    if (write_all(log->fd, header, sizeof(header)) < 0 ||
        write_all(log->fd, key, key_size) < 0 ||
        write_all(log->fd, value, value_size) < 0 || sync_file(log->fd) < 0) {
        log->failed = 1;
        return HT_IO_ERROR;
    }
    return HT_OK;
}

static ht_status replay_records(int fd, log_replay_fn replay, void *context) {
    off_t valid_end = 0;
    for (;;) {
        unsigned char header[HEADER_SIZE];
        ssize_t got = read_record(fd, header, sizeof(header));
        if (got < 0) return HT_IO_ERROR;
        if (got == 0) return HT_OK;
        if (got != HEADER_SIZE) break;
        size_t key_size = (size_t)decode(header + 8, 4);
        size_t value_size = (size_t)decode(header + 12, 4);
        uint64_t deadline = decode(header + 16, 8);
        int operation = header[4];
        if (memcmp(header, "MRD1", 4) != 0 || header[5] || header[6] || header[7] ||
            (operation != LOG_SET && operation != LOG_DELETE) ||
            key_size > MAX_PAYLOAD || value_size > MAX_PAYLOAD - key_size ||
            deadline > INT64_MAX ||
            decode(header + 24, 4) != checksum(UINT32_C(2166136261), header, 24) ||
            (operation == LOG_DELETE && (value_size || deadline)))
            return HT_CORRUPT_LOG;

        char *payload = malloc(key_size + value_size + 2);
        if (!payload) return HT_NO_MEMORY;
        got = read_record(fd, payload, key_size + value_size);
        if (got < 0) { free(payload); return HT_IO_ERROR; }
        if ((size_t)got != key_size + value_size) { free(payload); break; }
        uint32_t hash = checksum(UINT32_C(2166136261), payload, key_size + value_size);
        if (hash != decode(header + 28, 4) || memchr(payload, 0, key_size + value_size)) {
            free(payload);
            return HT_CORRUPT_LOG;
        }
        /* Make key and value separate NUL-terminated strings in the same buffer. */
        memmove(payload + key_size + 1, payload + key_size, value_size);
        payload[key_size] = 0;
        payload[key_size + 1 + value_size] = 0;
        ht_status status = replay(context, operation, payload,
                                  payload + key_size + 1, (int64_t)deadline);
        free(payload);
        if (status != HT_OK) return status;
        valid_end = lseek(fd, 0, SEEK_CUR);
        if (valid_end < 0) return HT_IO_ERROR;
    }
    /* Remove an incomplete tail before later appends, not merely ignore it. */
    if (ftruncate(fd, valid_end) < 0 || sync_file(fd) < 0) return HT_IO_ERROR;
    return HT_OK;
}

static ht_status sync_parent(const char *path) {
    char *parent = strdup(path);
    if (!parent) return HT_NO_MEMORY;
    char *slash = strrchr(parent, '/');
    if (!slash) { free(parent); parent = strdup("."); }
    else if (slash == parent) slash[1] = 0;
    else *slash = 0;
    if (!parent) return HT_NO_MEMORY;
    int fd = open(parent, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    free(parent);
    if (fd < 0) return HT_IO_ERROR;
    int result = sync_file(fd);
    close(fd);
    return result == 0 ? HT_OK : HT_IO_ERROR;
}

ht_status log_open(ht_log **out, const char *path, log_replay_fn replay, void *context) {
    *out = NULL;
    ht_log *log = malloc(sizeof(*log));
    if (!log) return HT_NO_MEMORY;
    log->fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    log->failed = 0;
    if (log->fd < 0) { free(log); return HT_IO_ERROR; }
    if (flock(log->fd, LOCK_EX | LOCK_NB) < 0) { log_close(log); return HT_IO_ERROR; }
    struct stat info;
    ht_status status = HT_IO_ERROR;
    if (fstat(log->fd, &info) == 0 && S_ISREG(info.st_mode))
        status = replay_records(log->fd, replay, context);
    if (status == HT_OK) status = sync_parent(path);
    if (status == HT_OK && lseek(log->fd, 0, SEEK_END) < 0) status = HT_IO_ERROR;
    if (status != HT_OK) { log_close(log); return status; }
    *out = log;
    return HT_OK;
}

void log_close(ht_log *log) {
    if (!log) return;
    close(log->fd);
    free(log);
}
