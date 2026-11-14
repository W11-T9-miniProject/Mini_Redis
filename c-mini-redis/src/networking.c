#include "server.h"
/* RESP2 parser: 0 incomplete, -1 malformed, 1 complete. */
int lineint(const unsigned char *p, size_t n, size_t *pos, int64_t *v) {
  size_t start = *pos, j = start;
  while (j < n && p[j] != '\r' && p[j] != '\n') {
    if (j - start > 22)
      return -1;
    j++;
  }
  if (j == n)
    return 0;
  if (p[j] != '\r')
    return -1;
  if (j + 1 >= n)
    return 0;
  if (p[j + 1] != '\n' || j == start || j - start > 20)
    return -1;
  char tmp[32];
  memcpy(tmp, p + start, j - start);
  tmp[j - start] = 0;
  Str s = {j - start, (unsigned char *)tmp};
  if (!integer(s, v))
    return -1;
  *pos = j + 2;
  return 1;
}
int parse(Buf *b, int *argc, Str *a, size_t *consumed) {
  size_t pos = 0;
  int64_t count;
  if (!b->n)
    return 0;
  if (b->p[pos++] != '*')
    return -1;
  int status = lineint(b->p, b->n, &pos, &count);
  if (status <= 0)
    return status;
  if (count < 1 || count > MAXARG)
    return -1;
  for (int64_t j = 0; j < count; j++) {
    if (pos >= b->n)
      return 0;
    if (b->p[pos++] != '$')
      return -1;
    int64_t len;
    status = lineint(b->p, b->n, &pos, &len);
    if (status <= 0)
      return status;
    if (len < 0 || len > MAXFRAME)
      return -1;
    if (pos + (size_t)len + 2 > MAXFRAME)
      return -1;
    if (pos + (size_t)len + 2 > b->n)
      return 0;
    if (b->p[pos + len] != '\r' || b->p[pos + len + 1] != '\n')
      return -1;
    a[j] = (Str){(size_t)len, b->p + pos};
    pos += (size_t)len + 2;
  }
  *argc = (int)count;
  *consumed = pos;
  return 1;
}

Client clients[MAXCLIENT];
void closeclient(Client *c) {
  ae_delete(eventloop, c->fd);
  close(c->fd);
  bfree(&c->in);
  bfree(&c->out);
  c->fd = -1;
}
void commands(Client *c) {
  int budget = 256;
  while (c->in.n && budget-- && c->out.n - c->out.off < MAXFRAME) {
    Str a[MAXARG], copies[MAXARG];
    int n;
    size_t consumed;
    int status = parse(&c->in, &n, a, &consumed);
    if (status == 0) {
      if (c->closing) {
        err(&c->out, "Protocol error: incomplete frame at EOF");
        c->in.n = 0;
      }
      break;
    }
    if (status < 0) {
      err(&c->out, "Protocol error");
      c->closing = 1;
      c->in.n = 0;
      break;
    }
    for (int j = 0; j < n; j++)
      copies[j] = dupstr(a[j].p, a[j].n);
    execute(n, copies, &c->out);
    for (int j = 0; j < n; j++)
      frees(copies[j]);
    memmove(c->in.p, c->in.p + consumed, c->in.n - consumed);
    c->in.n -= consumed;
  }
}
void nonblock(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  fcntl(fd, F_SETFD, FD_CLOEXEC);
}
void handler(int s) {
  (void)s;
  stopping = 1;
}
void usage(const char *argv0) {
  fprintf(stderr,
          "Usage: %s [--port 6379] [--bind 127.0.0.1] [--dir path] "
          "[--maxmemory bytes] [--snapshot-interval 60]\n",
          argv0);
}

void clientmask(Client *c) {
  int mask = 0;
  if (!c->closing && c->in.n < MAXFRAME && c->out.n - c->out.off < MAXFRAME)
    mask |= AE_READABLE;
  if (c->out.n > c->out.off)
    mask |= AE_WRITABLE;
  ae_set(eventloop, c->fd, mask, clientevent, c);
}
void clientevent(AeLoop *l, int fd, int mask, void *data) {
  (void)l;
  (void)fd;
  Client *c = data;
  if ((mask & AE_READABLE) && !c->closing) {
    unsigned char buf[65536];
    ssize_t n = recv(c->fd, buf, sizeof buf, 0);
    if (n > 0) {
      append(&c->in, buf, (size_t)n);
      commands(c);
      if (c->in.n >= MAXFRAME) {
        err(&c->out, "Protocol frame too large");
        c->closing = 1;
        c->in.n = 0;
      }
    } else if (n == 0) {
      c->closing = 1;
      commands(c);
    } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
      closeclient(c);
      return;
    }
  }
  if (c->out.n > c->out.off && (mask & AE_WRITABLE)) {
    ssize_t n = send(c->fd, c->out.p + c->out.off, c->out.n - c->out.off, 0);
    if (n > 0) {
      c->out.off += (size_t)n;
      if (c->out.off == c->out.n) {
        c->out.n = c->out.off = 0;
        if (c->out.cap > 65536)
          bfree(&c->out);
      }
    } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
      closeclient(c);
      return;
    }
  }
  if (c->closing && !c->out.n && !c->in.n) {
    closeclient(c);
    return;
  }
  clientmask(c);
}
void acceptclient(AeLoop *l, int listener, int mask, void *data) {
  (void)l;
  (void)mask;
  (void)data;
  for (int k = 0; k < 64; k++) {
    int fd = accept(listener, NULL, NULL);
    if (fd < 0)
      break;
    nonblock(fd);
    int j;
    for (j = 0; j < MAXCLIENT; j++)
      if (clients[j].fd < 0) {
        clients[j] = (Client){.fd = fd};
        clientmask(&clients[j]);
        break;
      }
    if (j == MAXCLIENT)
      close(fd);
  }
}
void networking_tick(void) {
  for (int j = 0; j < MAXCLIENT; j++)
    if (clients[j].fd >= 0) {
      Client *c = &clients[j];
      commands(c);
      if (c->closing && !c->out.n && !c->in.n)
        closeclient(c);
      else
        clientmask(c);
    }
}
