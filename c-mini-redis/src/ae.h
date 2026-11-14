#ifndef MINI_REDIS_AE_H
#define MINI_REDIS_AE_H
#include <stdint.h>
#define AE_READABLE 1
#define AE_WRITABLE 2
#define AE_CAPACITY 2048
#define AE_TIMERS 16
typedef struct AeLoop AeLoop;
typedef void (*AeFileProc)(AeLoop *, int, int, void *);
typedef void (*AeTimeProc)(AeLoop *, void *);
typedef struct {
  int fd, mask;
  AeFileProc callback;
  void *data;
} AeFileEvent;
typedef struct {
  int active, interval;
  int64_t next;
  AeTimeProc callback;
  void *data;
} AeTimeEvent;
struct AeLoop {
  AeFileEvent files[AE_CAPACITY];
  AeTimeEvent timers[AE_TIMERS];
  int stop;
};
void ae_init(AeLoop *loop);
int ae_set(AeLoop *loop, int fd, int mask, AeFileProc callback, void *data);
void ae_delete(AeLoop *loop, int fd);
int ae_timer(AeLoop *loop, int interval, AeTimeProc callback, void *data);
void ae_run(AeLoop *loop);
#endif
