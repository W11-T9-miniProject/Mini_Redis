#include "hash_table.h"
#include "json_input.h"

#include <arpa/inet.h>
#include <cjson/cJSON.h>
#include <errno.h>
#include <math.h>
#include <microhttpd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

enum { MAX_BODY = 65536 };

typedef struct {
    char *body;
    size_t size;
    unsigned int error;
} request_data;

static enum MHD_Result respond(struct MHD_Connection *connection, unsigned int code,
                                const char *status, const char *value) {
    cJSON *body = cJSON_CreateObject();
    if (!body) return MHD_NO;
    if (!cJSON_AddStringToObject(body, "status", status) ||
        (value && !cJSON_AddStringToObject(body, "value", value))) {
        cJSON_Delete(body);
        return MHD_NO;
    }
    char *json = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (!json) return MHD_NO;
    struct MHD_Response *response = MHD_create_response_from_buffer(
        strlen(json), json, MHD_RESPMEM_MUST_FREE);
    if (!response) { free(json); return MHD_NO; }
    MHD_add_response_header(response, "Content-Type", "application/json; charset=utf-8");
    MHD_add_response_header(response, "Cache-Control", "no-store");
    if (code == 405) MHD_add_response_header(response, "Allow", "POST");
    enum MHD_Result result = MHD_queue_response(connection, code, response);
    MHD_destroy_response(response);
    return result;
}

static enum MHD_Result storage_response(struct MHD_Connection *connection,
                                         ht_status status, const char *value) {
    switch (status) {
    case HT_OK: return respond(connection, 200, "ok", value);
    case HT_NOT_FOUND: return respond(connection, 404, "not_found", NULL);
    case HT_CONDITION_NOT_MET: return respond(connection, 409, "condition_not_met", NULL);
    case HT_INVALID_ARGUMENT: return respond(connection, 400, "invalid_argument", NULL);
    case HT_NO_MEMORY: return respond(connection, 503, "out_of_memory", NULL);
    case HT_IO_ERROR: return respond(connection, 503, "storage_io_error", NULL);
    case HT_CLOCK_ERROR: return respond(connection, 500, "clock_error", NULL);
    case HT_LOCK_ERROR: return respond(connection, 500, "lock_error", NULL);
    default: return respond(connection, 500, "internal_error", NULL);
    }
}

enum { F_OP = 1, F_KEY = 2, F_VALUE = 4, F_NX = 8,
       F_TTL = 16, F_UNTIL = 32, F_EXPECTED = 64 };

static unsigned int field_bit(const char *name) {
    const char *names[] = {"op", "key", "value", "nx", "ttl_ms", "expires_at_ms", "expected"};
    for (unsigned int i = 0; i < 7; ++i)
        if (strcmp(name, names[i]) == 0) return 1U << i;
    return 0;
}

static int integer_field(const cJSON *field, int64_t *value) {
    if (!cJSON_IsNumber(field) || !isfinite(field->valuedouble) ||
        field->valuedouble < 0 || field->valuedouble > 9007199254740991.0 ||
        floor(field->valuedouble) != field->valuedouble) return 0;
    *value = (int64_t)field->valuedouble;
    return 1;
}

static enum MHD_Result execute(hash_table *table, struct MHD_Connection *connection,
                                const request_data *request) {
    if (!request->body || !json_input_valid(request->body, request->size))
        return respond(connection, 400, "invalid_json", NULL);
    const char *end = NULL;
    cJSON *root = cJSON_ParseWithLengthOpts(request->body, request->size + 1, &end, 1);
    if (!root) return respond(connection, 400, "invalid_json", NULL);
    unsigned int seen = 0;
    int valid = cJSON_IsObject(root);
    for (cJSON *item = root->child; valid && item; item = item->next) {
        unsigned int bit = item->string ? field_bit(item->string) : 0;
        if (!bit || (seen & bit)) valid = 0;
        seen |= bit;
    }
    cJSON *op = cJSON_GetObjectItemCaseSensitive(root, "op");
    cJSON *key = cJSON_GetObjectItemCaseSensitive(root, "key");
    cJSON *value = cJSON_GetObjectItemCaseSensitive(root, "value");
    cJSON *expected = cJSON_GetObjectItemCaseSensitive(root, "expected");
    cJSON *nx = cJSON_GetObjectItemCaseSensitive(root, "nx");
    cJSON *ttl = cJSON_GetObjectItemCaseSensitive(root, "ttl_ms");
    cJSON *until = cJSON_GetObjectItemCaseSensitive(root, "expires_at_ms");
    if (!cJSON_IsString(op) || !cJSON_IsString(key)) valid = 0;
    unsigned int allowed = F_OP | F_KEY;
    int command = 0;
    if (valid) {
        if (strcmp(op->valuestring, "get") == 0) command = 1;
        else if (strcmp(op->valuestring, "delete") == 0) command = 2;
        else if (strcmp(op->valuestring, "set") == 0) {
            command = 3;
            allowed |= F_VALUE | F_NX | F_TTL | F_UNTIL;
            if (!cJSON_IsString(value) || (nx && !cJSON_IsBool(nx)) ||
                (ttl && until) || (cJSON_IsTrue(nx) && until)) valid = 0;
        } else if (strcmp(op->valuestring, "compare_delete") == 0) {
            command = 4;
            allowed |= F_EXPECTED;
            if (!cJSON_IsString(expected)) valid = 0;
        } else if (strcmp(op->valuestring, "compare_set") == 0) {
            command = 5;
            allowed |= F_EXPECTED | F_VALUE | F_UNTIL;
            if (!cJSON_IsString(expected) || !cJSON_IsString(value) || !until) valid = 0;
        } else valid = 0;
    }
    int64_t ttl_ms = 0, deadline = 0;
    if ((seen & ~allowed) || (ttl && !integer_field(ttl, &ttl_ms)) ||
        (until && !integer_field(until, &deadline))) valid = 0;
    if (!valid) {
        cJSON_Delete(root);
        return respond(connection, 400, "invalid_argument", NULL);
    }
    ht_status status;
    char *output = NULL;
    const char *k = key->valuestring;
    switch (command) {
    case 1: status = ht_get(table, k, &output); break;
    case 2: status = ht_delete(table, k); break;
    case 3:
        if (cJSON_IsTrue(nx)) status = ht_set_nx(table, k, value->valuestring, ttl_ms);
        else if (until) status = ht_set_until(table, k, value->valuestring, deadline);
        else if (ttl_ms) status = ht_set_ttl(table, k, value->valuestring, ttl_ms);
        else status = ht_set(table, k, value->valuestring);
        break;
    case 4: status = ht_compare_delete(table, k, expected->valuestring); break;
    default: status = ht_compare_set_until(table, k, expected->valuestring,
                                           value->valuestring, deadline); break;
    }
    cJSON_Delete(root);
    enum MHD_Result result = storage_response(connection, status, output);
    free(output);
    return result;
}

static enum MHD_Result handle_request(void *context, struct MHD_Connection *connection,
                                       const char *url, const char *method,
                                       const char *version, const char *upload,
                                       size_t *upload_size, void **request_context) {
    (void)version;
    request_data *request = *request_context;
    if (!request) {
        request = calloc(1, sizeof(*request));
        if (!request) return MHD_NO;
        *request_context = request;
        return MHD_YES;
    }
    if (*upload_size) {
        if (!request->error) {
            if (*upload_size > MAX_BODY - request->size) request->error = 413;
            else {
                char *body = realloc(request->body, request->size + *upload_size + 1);
                if (!body) request->error = 503;
                else {
                    request->body = body;
                    memcpy(body + request->size, upload, *upload_size);
                    request->size += *upload_size;
                    body[request->size] = 0;
                }
            }
        }
        *upload_size = 0;
        return MHD_YES;
    }
    if (request->error)
        return respond(connection, request->error,
                       request->error == 413 ? "request_too_large" : "out_of_memory", NULL);
    if (strcmp(url, "/health") == 0 && strcmp(method, "GET") == 0)
        return respond(connection, 200, "ok", NULL);
    if (strcmp(url, "/kv") != 0) return respond(connection, 404, "unknown_endpoint", NULL);
    if (strcmp(method, "POST") != 0) return respond(connection, 405, "method_not_allowed", NULL);
    const char *type = MHD_lookup_connection_value(connection, MHD_HEADER_KIND, "Content-Type");
    if (!type || strncasecmp(type, "application/json", 16) != 0)
        return respond(connection, 415, "unsupported_media_type", NULL);
    const char *suffix = type + 16;
    while (*suffix == ' ' || *suffix == '\t') ++suffix;
    if (*suffix && *suffix != ';')
        return respond(connection, 415, "unsupported_media_type", NULL);
    return execute(context, connection, request);
}

static void completed(void *context, struct MHD_Connection *connection,
                       void **request_context, enum MHD_RequestTerminationCode reason) {
    (void)context; (void)connection; (void)reason;
    request_data *request = *request_context;
    if (request) { free(request->body); free(request); *request_context = NULL; }
}

int main(int argc, char **argv) {
    unsigned long port = 8080;
    if (argc > 3) { fputs("Usage: mini-redis-server [port] [log_path]\n", stderr); return 1; }
    if (argc >= 2) {
        char *end;
        errno = 0;
        port = strtoul(argv[1], &end, 10);
        if (!*argv[1] || argv[1][0] == '-' || *end || errno || port > 65535) {
            fputs("Invalid port\n", stderr); return 1;
        }
    }
    const char *path = argc == 3 ? argv[2] : "data/mini-redis.aof";
    if (argc < 3 && mkdir("data", 0700) < 0 && errno != EEXIST) { perror("data"); return 1; }
    sigset_t signals;
    sigemptyset(&signals);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGTERM);
    if (pthread_sigmask(SIG_BLOCK, &signals, NULL) != 0) return 1;
    signal(SIGPIPE, SIG_IGN);
    hash_table table;
    ht_status status = ht_open(&table, path);
    if (status != HT_OK) { fprintf(stderr, "Cannot recover log (status %d): %s\n", status, path); return 1; }
    struct sockaddr_in address = {0};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons((uint16_t)port);
    struct MHD_Daemon *daemon = MHD_start_daemon(
        MHD_USE_INTERNAL_POLLING_THREAD | MHD_USE_ERROR_LOG, (uint16_t)port,
        NULL, NULL, handle_request, &table,
        MHD_OPTION_SOCK_ADDR, &address,
        MHD_OPTION_THREAD_POOL_SIZE, (unsigned int)4,
        MHD_OPTION_CONNECTION_LIMIT, (unsigned int)256,
        MHD_OPTION_CONNECTION_TIMEOUT, (unsigned int)10,
        MHD_OPTION_NOTIFY_COMPLETED, completed, NULL,
        MHD_OPTION_END);
    if (!daemon) { fputs("Cannot start HTTP listener\n", stderr); ht_destroy(&table); return 1; }
    const union MHD_DaemonInfo *info = MHD_get_daemon_info(daemon, MHD_DAEMON_INFO_BIND_PORT);
    if (!info) { MHD_stop_daemon(daemon); ht_destroy(&table); return 1; }
    printf("LISTENING 127.0.0.1:%u\n", (unsigned int)info->port);
    fflush(stdout);
    int received;
    int result = sigwait(&signals, &received);
    MHD_stop_daemon(daemon);
    ht_destroy(&table);
    return result == 0 ? 0 : 1;
}
