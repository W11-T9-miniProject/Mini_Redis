/* Linux에서는 clock_gettime 등 POSIX 선언을 표준 C 헤더보다 먼저 활성화한다. */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#include "mini_redis/server.h"
#include "mini_redis/command.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
typedef SOCKET Socket;
typedef WSAPOLLFD PollFd;
#define BAD_SOCKET INVALID_SOCKET
#define native_poll(fds, count, timeout) WSAPoll((fds), (ULONG)(count), (timeout))
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
typedef int Socket;
typedef struct pollfd PollFd;
#define BAD_SOCKET (-1)
#define native_poll(fds, count, timeout) poll((fds), (nfds_t)(count), (timeout))
#endif

#define COMMANDS_PER_TURN 64u
#define READ_CHUNK 65536u
#define ACCEPTS_PER_TURN 32u

/* signal handler에서 printf/free/socket 정리를 하면 안전하지 않다.
 * 플래그만 기록하고 실제 자원 정리는 평상시의 이벤트 루프에서 수행한다. */
static volatile sig_atomic_t stopping;

static void stop_signal(int number) {
    (void)number;
    stopping = 1;
}

static void close_socket(Socket fd) {
#ifdef _WIN32
    closesocket(fd);
#else
    close(fd);
#endif
}

static bool interrupted(void) {
#ifdef _WIN32
    return WSAGetLastError() == WSAEINTR;
#else
    return errno == EINTR;
#endif
}

static bool would_block(void) {
#ifdef _WIN32
    return WSAGetLastError() == WSAEWOULDBLOCK;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK;
#endif
}

static bool nonblocking(Socket fd) {
#ifdef _WIN32
    u_long enabled = 1;
    return ioctlsocket(fd, FIONBIO, &enabled) == 0;
#else
    int flags = fcntl(fd, F_GETFL, 0);
    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

static int64_t monotonic_ms(void) {
#ifdef _WIN32
    return (int64_t)GetTickCount64();
#else
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return -1;
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
#endif
}

static void close_client(Client *client) {
    if (client->fd != -1) close_socket((Socket)client->fd);
    free(client->input);
    free(client->output);
    memset(client, 0, sizeof(*client));
    client->fd = -1;
}

/* realloc 성공 전에는 원래 포인터를 변경하지 않는다.
 * capacity는 할당 크기, len은 유효한 데이터 길이로 서로 다르다. */
static bool reserve(unsigned char **data, size_t *capacity, size_t needed, size_t limit) {
    if (needed > limit) return false;
    if (*capacity >= needed) return true;
    size_t next = *capacity ? *capacity : 4096;
    while (next < needed) next = next > limit / 2 ? limit : next * 2;
    unsigned char *copy = mr_realloc(*data, next);
    if (!copy) return false;
    *data = copy;
    *capacity = next;
    return true;
}

static bool queue_reply(Client *client, MrSlice reply) {
    /* 이미 보낸 앞부분을 먼저 버려 느린 클라이언트의 대기량만 계산한다. */
    if (client->output_sent) {
        size_t left = client->output_len - client->output_sent;
        memmove(client->output, client->output + client->output_sent, left);
        client->output_len = left;
        client->output_sent = 0;
    }
    if (reply.len > MR_MAX_QUEUED_BYTES - client->output_len) return false;
    size_t needed = client->output_len + reply.len;
    if (!reserve(&client->output, &client->output_capacity, needed, MR_MAX_QUEUED_BYTES)) return false;
    if (reply.len) memcpy(client->output + client->output_len, reply.data, reply.len);
    client->output_len = needed;
    return true;
}

static bool protocol_error(Client *client, const char *message) {
    MrBuffer reply = {0};
    MrStatus status = resp_error(mr_slice(message), &reply);
    bool queued = status == MR_OK && queue_reply(client, (MrSlice){reply.data, reply.len});
    mr_buffer_free(&reply);
    client->input_len = 0;
    client->pending_commands = false;
    client->read_closed = true;
    client->close_after_flush = true;
    return queued;
}

/* 수신 버퍼 → RESP → command → Store → HashTable → RESP 응답 → 출력 큐.
 * 이 함수와 store_sweep는 같은 스레드에서만 호출하므로 저장소 접근이 직렬화된다. */
static bool process_input(Client *client, Store *store) {
    client->pending_commands = false;
    if (client->close_after_flush) return true;
    size_t consumed_total = 0;
    unsigned commands = 0;
    while (consumed_total < client->input_len && commands < COMMANDS_PER_TURN) {
        RespCommand command;
        size_t consumed = 0;
        MrSlice input = {client->input + consumed_total, client->input_len - consumed_total};
        MrStatus status = resp_parse(input, &command, &consumed);
        if (status == MR_NEED_MORE) {
            if (client->read_closed) return protocol_error(client, "truncated request");
            break;
        }
        if (status != MR_OK) return protocol_error(client, status == MR_LIMIT ? "request too large" : "invalid RESP");
        MrBuffer reply = {0};
        status = command_execute(store, &command, monotonic_ms(), &reply);
        if (status != MR_OK) { mr_buffer_free(&reply); return false; }
        bool queued = queue_reply(client, (MrSlice){reply.data, reply.len});
        mr_buffer_free(&reply);
        if (!queued) return false; /* 출력 상한 초과/OOM은 해당 연결만 닫는다. */
        consumed_total += consumed;
        ++commands;
    }
    /* args는 입력 버퍼를 참조한다. command_execute를 끝낸 다음에만 이동한다. */
    if (consumed_total) {
        client->input_len -= consumed_total;
        memmove(client->input, client->input + consumed_total, client->input_len);
    }
    client->pending_commands = commands == COMMANDS_PER_TURN && client->input_len > 0;
    return true;
}

static bool read_client(Client *client, Store *store) {
    if (client->read_closed) return true;
    if (client->input_len == MR_MAX_FRAME_BYTES)
        return protocol_error(client, "request too large");
    size_t room = MR_MAX_FRAME_BYTES - client->input_len;
    if (room > READ_CHUNK) room = READ_CHUNK;
    if (!reserve(&client->input, &client->input_capacity, client->input_len + room, MR_MAX_FRAME_BYTES)) return false;
    int received;
    do {
        received = (int)recv((Socket)client->fd, (char *)client->input + client->input_len, (int)room, 0);
    } while (received < 0 && interrupted());
    if (received < 0) return would_block();
    if (received == 0) client->read_closed = true; /* 쓰기만 닫은 peer도 응답은 받는다. */
    else client->input_len += (size_t)received;
    return process_input(client, store);
}

static bool flush_client(Client *client) {
    if (client->output_sent == client->output_len) return true;
    size_t remaining = client->output_len - client->output_sent;
    if (remaining > READ_CHUNK) remaining = READ_CHUNK;
    int flags = 0;
#ifdef MSG_NOSIGNAL
    flags = MSG_NOSIGNAL;
#endif
    int sent;
    do {
        sent = (int)send((Socket)client->fd, (const char *)client->output + client->output_sent, (int)remaining, flags);
    } while (sent < 0 && interrupted());
    if (sent < 0) return would_block();
    if (sent == 0) return false;
    client->output_sent += (size_t)sent;
    if (client->output_sent == client->output_len) client->output_sent = client->output_len = 0;
    return true;
}

static Socket open_listener(const ServerConfig *config) {
    Socket fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd == BAD_SOCKET) return BAD_SOCKET;
    int enabled = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&enabled, (int)sizeof(enabled)) != 0) {
        close_socket(fd);
        return BAD_SOCKET;
    }
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(config->port);
    if (inet_pton(AF_INET, config->bind_address, &address.sin_addr) != 1 ||
        !nonblocking(fd) || bind(fd, (struct sockaddr *)&address, (int)sizeof(address)) != 0 ||
        listen(fd, MR_MAX_CLIENTS) != 0) {
        close_socket(fd);
        return BAD_SOCKET;
    }
    return fd;
}

static void accept_clients(Socket listener, Client *clients) {
    for (unsigned accepted = 0; accepted < ACCEPTS_PER_TURN; ++accepted) {
        Socket fd = accept(listener, NULL, NULL);
        if (fd == BAD_SOCKET) {
            if (interrupted()) continue;
            break;
        }
        size_t index = 0;
        while (index < MR_MAX_CLIENTS && clients[index].fd != -1) ++index;
        if (index == MR_MAX_CLIENTS || !nonblocking(fd)) { close_socket(fd); continue; }
        clients[index].fd = (intptr_t)fd;
    }
}

MrStatus server_run(const ServerConfig *config) {
    if (!config || !config->bind_address || !config->port) return MR_INVALID;
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return MR_IO;
#endif
    Store store = {0};
    MrStatus status = store_init(&store, config->initial_capacity);
    if (status != MR_OK) goto platform_cleanup;
    Socket listener = open_listener(config);
    if (listener == BAD_SOCKET) { status = MR_IO; goto store_cleanup; }
    Client clients[MR_MAX_CLIENTS];
    memset(clients, 0, sizeof(clients));
    for (size_t i = 0; i < MR_MAX_CLIENTS; ++i) clients[i].fd = -1;
    stopping = 0;
    void (*old_int)(int) = signal(SIGINT, stop_signal);
    void (*old_term)(int) = signal(SIGTERM, stop_signal);
#ifndef _WIN32
    void (*old_pipe)(int) = signal(SIGPIPE, SIG_IGN);
#endif
    printf("Mini Redis listening on %s:%u (single event loop)\n", config->bind_address, (unsigned)config->port);
    fflush(stdout);

    while (!stopping) {
        PollFd fds[MR_MAX_CLIENTS + 1];
        size_t mapping[MR_MAX_CLIENTS + 1];
        size_t count = 1;
        bool pending = false;
        memset(fds, 0, sizeof(fds));
        fds[0].fd = listener;
        fds[0].events = POLLIN;
        for (size_t i = 0; i < MR_MAX_CLIENTS; ++i) {
            Client *client = &clients[i];
            if (client->fd == -1) continue;
            /* 예산 때문에 남긴 pipeline은 새 recv 이벤트 없이도 이어서 처리한다. */
            if (client->pending_commands && !process_input(client, &store)) { close_client(client); continue; }
            bool output_empty = client->output_sent == client->output_len;
            if ((client->close_after_flush || client->read_closed) &&
                !client->pending_commands && output_empty) { close_client(client); continue; }
            pending = pending || client->pending_commands;
            fds[count].fd = (Socket)client->fd;
            fds[count].events = (short)((client->read_closed ? 0 : POLLIN) | (output_empty ? 0 : POLLOUT));
            mapping[count++] = i;
        }
        int ready = native_poll(fds, count, pending ? 0 : 25);
        if (ready < 0) {
            if (interrupted()) continue;
            status = MR_IO;
            break;
        }
        if (fds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) { status = MR_IO; break; }
        if (fds[0].revents & POLLIN) accept_clients(listener, clients);
        for (size_t i = 1; i < count; ++i) {
            Client *client = &clients[mapping[i]];
            short events = fds[i].revents;
            if (events & POLLNVAL) { close_client(client); continue; }
            if ((events & (POLLIN | POLLHUP)) && !read_client(client, &store)) {
                close_client(client); continue;
            }
            if ((events & POLLOUT) && !flush_client(client)) { close_client(client); continue; }
            if (events & POLLERR) close_client(client);
        }
        /* poll timeout가 있어 요청이 전혀 없어도 만료 정리가 실행된다. */
        size_t deleted = 0;
        status = store_sweep(&store, monotonic_ms(), config->sweep_budget, &deleted);
        if (status != MR_OK) break;
    }
    for (size_t i = 0; i < MR_MAX_CLIENTS; ++i) close_client(&clients[i]);
    close_socket(listener);
    if (old_int != SIG_ERR) signal(SIGINT, old_int);
    if (old_term != SIG_ERR) signal(SIGTERM, old_term);
#ifndef _WIN32
    if (old_pipe != SIG_ERR) signal(SIGPIPE, old_pipe);
#endif
store_cleanup:
    store_destroy(&store);
platform_cleanup:
#ifdef _WIN32
    WSACleanup();
#endif
    return status;
}
