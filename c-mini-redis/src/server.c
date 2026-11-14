#include "server.h"
AeLoop *eventloop;
static void servercron(AeLoop *l, void *data) {
  (void)data;
  static int64_t lastsave;
  networking_tick();
  expirecycle();
  if (snapshot_pid > 0) {
    int status;
    pid_t p = waitpid(snapshot_pid, &status, WNOHANG);
    if (p == snapshot_pid) {
      if (!WIFEXITED(status) || WEXITSTATUS(status))
        fprintf(stderr, "background snapshot failed\n");
      snapshot_pid = -1;
    }
  }
  if (!lastsave)
    lastsave = nowms();
  if (nowms() - lastsave >= snapshot_interval * 1000LL) {
    backgroundsnapshot();
    lastsave = nowms();
  }
  if (stopping)
    l->stop = 1;
}
int main(int argc, char **argv) {
  int port = 6379;
  const char *bindaddr = "127.0.0.1";
  for (int i = 1; i < argc; i++) {
    if (i + 1 >= argc) {
      usage(argv[0]);
      return 1;
    }
    char *end;
    errno = 0;
    if (!strcmp(argv[i], "--dir")) {
      if (strlen(argv[++i]) >= sizeof datadir)
        return 1;
      strcpy(datadir, argv[i]);
    } else if (!strcmp(argv[i], "--bind"))
      bindaddr = argv[++i];
    else if (!strcmp(argv[i], "--port")) {
      long p = strtol(argv[++i], &end, 10);
      if (errno || *end || p < 1 || p > 65535)
        return 1;
      port = (int)p;
    } else if (!strcmp(argv[i], "--maxmemory")) {
      unsigned long long m = strtoull(argv[++i], &end, 10);
      if (errno || *end || m < 1024 || m > SIZE_MAX)
        return 1;
      maxmemory = (size_t)m;
    } else if (!strcmp(argv[i], "--snapshot-interval")) {
      long sec = strtol(argv[++i], &end, 10);
      if (errno || *end || sec < 1 || sec > 86400)
        return 1;
      snapshot_interval = (int)sec;
    } else {
      usage(argv[0]);
      return 1;
    }
  }
  if (mkdir(datadir, 0700) && errno != EEXIST) {
    perror("mkdir");
    return 1;
  }
  if (!loadfile())
    return 1;
  signal(SIGPIPE, SIG_IGN);
  signal(SIGTERM, handler);
  signal(SIGINT, handler);
  srand((unsigned)time(NULL));
  int listener = socket(AF_INET, SOCK_STREAM, 0);
  if (listener < 0) {
    perror("socket");
    return 1;
  }
  int one = 1;
  setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  struct sockaddr_in addr = {.sin_family = AF_INET,
                             .sin_port = htons((uint16_t)port)};
  if (inet_pton(AF_INET, bindaddr, &addr.sin_addr) != 1) {
    fprintf(stderr, "invalid bind address\n");
    return 1;
  }
  if (bind(listener, (struct sockaddr *)&addr, sizeof addr) ||
      listen(listener, 128)) {
    perror("listen");
    return 1;
  }
  nonblock(listener);
  for (int j = 0; j < MAXCLIENT; j++)
    clients[j].fd = -1;
  fprintf(stderr, "mini-redis listening %s:%d, maxmemory=%zu\n", bindaddr, port,
          maxmemory);
  AeLoop loop;
  eventloop = &loop;
  ae_init(&loop);
  ae_set(&loop, listener, AE_READABLE, acceptclient, NULL);
  ae_timer(&loop, 10, servercron, NULL);
  ae_run(&loop);
  close(listener);
  for (int j = 0; j < MAXCLIENT; j++)
    if (clients[j].fd >= 0)
      closeclient(&clients[j]);
  if (snapshot_pid > 0)
    waitpid(snapshot_pid, NULL, 0);
  int saved = savefile();
  flush();
  return saved ? 0 : 1;
}
