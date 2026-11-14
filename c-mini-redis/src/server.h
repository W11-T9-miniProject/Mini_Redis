#ifndef MINI_REDIS_SERVER_H
#define MINI_REDIS_SERVER_H
#define _POSIX_C_SOURCE 200809L
#include "ae.h"
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <lauxlib.h>
#include <limits.h>
#include <lua.h>
#include <math.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
/* All mutable database state belongs to the single AE thread. */
#define BUCKETS 65536
#define CBUCKETS 64
#define LEVELS 16
#define MAXARG 4096
#define MAXFRAME (16 * 1024 * 1024)
#define MAXCLIENT 1024
#define DEFAULT_TTL 300000

typedef struct {
  size_t n;
  unsigned char *p;
} Str;
typedef struct Buf {
  unsigned char *p;
  size_t n, cap, off;
} Buf;
typedef struct Item {
  Str k, v;
  double score;
  struct Item *next, *prev, *hnext;
  struct Item *forward[LEVELS];
  int level;
} Item;
typedef struct Obj {
  int type;
  size_t count;
  Str str;
  Item *head, *tail;
  Item *buckets[CBUCKETS];
  Item *skip[LEVELS];
} Obj;
typedef struct Entry {
  Str key;
  Obj *o;
  int64_t expires, access;
  size_t slot;
  struct Entry *next;
} Entry;
typedef struct LuaCtx {
  int nkeys, nargs;
  Str *keys, *args;
  size_t bytes;
  int calls;
  int64_t deadline;
  int expired;
  Buf scratch;
} LuaCtx;
typedef struct Client {
  int fd, closing;
  Buf in, out;
} Client;
enum { STRING = 1, LIST, SET, HASH, ZSET };
extern size_t used_memory, maxmemory, dbsize;
extern Entry **keyslots;
extern volatile sig_atomic_t stopping;
extern Entry *db[BUCKETS];
extern char datadir[PATH_MAX];
extern pid_t snapshot_pid;
extern int snapshot_interval;
extern Client clients[MAXCLIENT];
extern AeLoop *eventloop;
void acceptclient(AeLoop *, int, int, void *);
void networking_tick(void);
void clientevent(AeLoop *, int, int, void *);
int64_t nowms(void);
void *xm(size_t n);
void xf(void *p, size_t n);
Str dupstr(const void *p, size_t n);
void frees(Str s);
int eq(Str a, Str b);
int cmp(Str a, Str b);
uint64_t hash(Str s);
int word(Str s, const char *t);
int integer(Str s, int64_t *v);
int realnum(Str s, double *v);
void append(Buf *b, const void *p, size_t n);
void raw(Buf *b, const char *s);
void num(Buf *b, char t, int64_t n);
void bulk(Buf *b, Str s);
void null(Buf *b);
void err(Buf *b, const char *s);
void wrong(Buf *b);
void bfree(Buf *b);
Obj *newobj(int type);
void freeitem(Item *i);
void freeobj(Obj *o);
void delentry(Entry *e);
Entry *lookup(Str k);
Entry *create(Str k, int type);
void flush(void);
Item *finditem(Obj *o, Str k);
Item *additem(Obj *o, Str k, Str *v);
int before(Item *a, double score, Str k);
void zinsert(Obj *o, Item *i);
void zunlink(Obj *o, Item *i);
void removeitem(Obj *o, Item *i);
void expirecycle(void);
void evict(void);
int arity(Buf *out, int ok);
int validttl(Str s, int64_t *v, Buf *b, int positive);
int gettype(Entry *e, int type, Buf *out);
void execute(int n, Str *a, Buf *out);
void *luaalloc(void *ud, void *ptr, size_t old, size_t n);
LuaCtx *luactx(lua_State *L);
void luahook(lua_State *L, lua_Debug *d);
int readonly(lua_State *L);
int keyindex(lua_State *L);
int rediscall(lua_State *L);
int scriptcheck(Str s);
void keyproxy(lua_State *L, const char *name, int kind);
void evalcmd(int n, Str *a, Buf *out);
int wr(FILE *f, const void *p, size_t n);
int byte(FILE *f, unsigned char x);
int lengthwrite(FILE *f, uint64_t n);
int stringwrite(FILE *f, Str s);
int littlewrite(FILE *f, uint64_t n);
int savefile(void);
int rd(FILE *f, void *p, size_t n);
int lengthread(FILE *f, uint64_t *n);
int stringread(FILE *f, Str *s);
int littleread(FILE *f, uint64_t *v);
int loadfile(void);
void backgroundsnapshot(void);
int lineint(const unsigned char *p, size_t n, size_t *pos, int64_t *v);
int parse(Buf *b, int *argc, Str *a, size_t *consumed);
void closeclient(Client *c);
void commands(Client *c);
void nonblock(int fd);
void handler(int s);
void usage(const char *argv0);
#endif
