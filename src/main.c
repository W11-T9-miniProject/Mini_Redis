#include "mini_redis/server.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    ServerConfig config = {"127.0.0.1", 6379, 16, 64};
    if (argc > 3) {
        fprintf(stderr, "Usage: %s [port] [bind-address]\n", argv[0]);
        return 2;
    }
    if (argc >= 2) {
        char *end = NULL;
        errno = 0;
        long port = strtol(argv[1], &end, 10);
        if (errno || end == argv[1] || *end || port < 1 || port > 65535) {
            fprintf(stderr, "Invalid port\n");
            return 2;
        }
        config.port = (uint16_t)port;
    }
    if (argc == 3) config.bind_address = argv[2];
    MrStatus status = server_run(&config);
    if (status != MR_OK) {
        fprintf(stderr, "Server stopped: %s\n", mr_status_name(status));
        return 1;
    }
    return 0;
}
