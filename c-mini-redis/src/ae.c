#define _POSIX_C_SOURCE 200809L
#include "ae.h"
#include <errno.h>
#include <poll.h>
#include <string.h>
#include <time.h>
static int64_t monotonic_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
void ae_init(AeLoop *l) {
  memset(l, 0, sizeof *l);
  for (int i = 0; i < AE_CAPACITY; i++)
    l->files[i].fd = -1;
}
int ae_set(AeLoop *l, int fd, int mask, AeFileProc cb, void *data) {
  int empty = -1;
  for (int i = 0; i < AE_CAPACITY; i++) {
    if (l->files[i].fd == fd) {
      l->files[i] = (AeFileEvent){fd, mask, cb, data};
      return 0;
    }
    if (l->files[i].fd < 0 && empty < 0)
      empty = i;
  }
  if (empty < 0)
    return -1;
  l->files[empty] = (AeFileEvent){fd, mask, cb, data};
  return 0;
}
void ae_delete(AeLoop *l, int fd) {
  for (int i = 0; i < AE_CAPACITY; i++)
    if (l->files[i].fd == fd) {
      l->files[i].fd = -1;
      return;
    }
}
int ae_timer(AeLoop *l, int interval, AeTimeProc cb, void *data) {
  if (interval < 1)
    return -1;
  for (int i = 0; i < AE_TIMERS; i++)
    if (!l->timers[i].active) {
      l->timers[i] =
          (AeTimeEvent){1, interval, monotonic_ms() + interval, cb, data};
      return i;
    }
  return -1;
}
void ae_run(AeLoop *l) {
  while (!l->stop) {
    struct pollfd p[AE_CAPACITY];
    AeFileEvent saved[AE_CAPACITY];
    int count = 0;
    for (int i = 0; i < AE_CAPACITY; i++)
      if (l->files[i].fd >= 0 && l->files[i].mask) {
        AeFileEvent e = l->files[i];
        p[count] =
            (struct pollfd){e.fd,
                            (short)(((e.mask & AE_READABLE) ? POLLIN : 0) |
                                    ((e.mask & AE_WRITABLE) ? POLLOUT : 0)),
                            0};
        saved[count++] = e;
      }
    int64_t now = monotonic_ms(), wait = 1000;
    for (int i = 0; i < AE_TIMERS; i++)
      if (l->timers[i].active && l->timers[i].next - now < wait)
        wait = l->timers[i].next - now;
    if (wait < 0)
      wait = 0;
    int result = poll(p, (nfds_t)count, (int)wait);
    if (result < 0 && errno != EINTR) {
      l->stop = 1;
      return;
    }
    for (int i = 0; i < count; i++)
      if (p[i].revents) {
        int valid = 0;
        for (int j = 0; j < AE_CAPACITY; j++)
          if (l->files[j].fd == saved[i].fd &&
              l->files[j].data == saved[i].data) {
            valid = 1;
            break;
          }
        if (!valid)
          continue;
        int mask = 0;
        if (p[i].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL))
          mask |= AE_READABLE;
        if (p[i].revents & (POLLOUT | POLLERR | POLLNVAL))
          mask |= AE_WRITABLE;
        saved[i].callback(l, saved[i].fd, mask, saved[i].data);
      }
    now = monotonic_ms();
    for (int i = 0; i < AE_TIMERS; i++)
      if (l->timers[i].active && l->timers[i].next <= now) {
        l->timers[i].next = now + l->timers[i].interval;
        l->timers[i].callback(l, l->timers[i].data);
      }
  }
}
