#include "server.h"
size_t used_memory = 0, maxmemory = 64 * 1024 * 1024;
volatile sig_atomic_t stopping = 0;
int64_t nowms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void *xm(size_t n) {
  void *p = malloc(n ? n : 1);
  if (!p) {
    perror("malloc");
    exit(1);
  }
  used_memory += n;
  return p;
}
void xf(void *p, size_t n) {
  if (p) {
    free(p);
    used_memory -= n;
  }
}
Str dupstr(const void *p, size_t n) {
  Str s = {n, xm(n + 1)};
  memcpy(s.p, p, n);
  s.p[n] = 0;
  return s;
}
void frees(Str s) { xf(s.p, s.n + 1); }
int eq(Str a, Str b) { return a.n == b.n && !memcmp(a.p, b.p, a.n); }
int cmp(Str a, Str b) {
  size_t n = a.n < b.n ? a.n : b.n;
  int c = memcmp(a.p, b.p, n);
  return c ? c : (a.n > b.n) - (a.n < b.n);
}
uint64_t hash(Str s) {
  uint64_t h = 1469598103934665603ULL;
  for (size_t i = 0; i < s.n; i++) {
    h ^= s.p[i];
    h *= 1099511628211ULL;
  }
  return h;
}
int word(Str s, const char *t) {
  size_t n = strlen(t);
  if (s.n != n)
    return 0;
  for (size_t i = 0; i < n; i++)
    if ((s.p[i] >= 'a' && s.p[i] <= 'z' ? s.p[i] - 32 : s.p[i]) !=
        (unsigned char)t[i])
      return 0;
  return 1;
}
int integer(Str s, int64_t *v) {
  if (!s.n || s.n > 20)
    return 0;
  size_t first = s.p[0] == '-' ? 1 : 0;
  if (first == s.n)
    return 0;
  if (s.p[first] == '0' && (s.n - first > 1 || first))
    return 0;
  for (size_t j = first; j < s.n; j++)
    if (s.p[j] < '0' || s.p[j] > '9')
      return 0;
  char *e;
  errno = 0;
  long long x = strtoll((char *)s.p, &e, 10);
  if (errno || e != (char *)s.p + s.n)
    return 0;
  *v = x;
  return 1;
}
int realnum(Str s, double *v) {
  if (!s.n || memchr(s.p, 0, s.n) || s.p[0] <= ' ')
    return 0;
  char *e;
  errno = 0;
  double d = strtod((char *)s.p, &e);
  if (errno || e != (char *)s.p + s.n || isnan(d))
    return 0;
  *v = d;
  return 1;
}

void append(Buf *b, const void *p, size_t n) {
  if (!n)
    return;
  if (b->off) {
    memmove(b->p, b->p + b->off, b->n - b->off);
    b->n -= b->off;
    b->off = 0;
  }
  if (b->n + n > b->cap) {
    size_t c = b->cap ? b->cap : 256;
    while (c < b->n + n)
      c *= 2;
    unsigned char *q = realloc(b->p, c);
    if (!q)
      exit(1);
    b->p = q;
    b->cap = c;
  }
  memcpy(b->p + b->n, p, n);
  b->n += n;
}
void raw(Buf *b, const char *s) { append(b, s, strlen(s)); }
void num(Buf *b, char t, int64_t n) {
  char s[64];
  int k = snprintf(s, sizeof s, "%c%lld\r\n", t, (long long)n);
  append(b, s, (size_t)k);
}
void bulk(Buf *b, Str s) {
  num(b, '$', (int64_t)s.n);
  append(b, s.p, s.n);
  raw(b, "\r\n");
}
void null(Buf *b) { raw(b, "$-1\r\n"); }
void err(Buf *b, const char *s) {
  raw(b, "-ERR ");
  for (; *s; s++) {
    char c = (*s == '\r' || *s == '\n') ? ' ' : *s;
    append(b, &c, 1);
  }
  raw(b, "\r\n");
}
void wrong(Buf *b) {
  raw(b,
      "-WRONGTYPE Operation against a key holding the wrong kind of value\r\n");
}
void bfree(Buf *b) {
  free(b->p);
  memset(b, 0, sizeof *b);
}
