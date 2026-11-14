#include "server.h"
Entry **keyslots;
static size_t keycapacity;
void delentry(Entry *e) {
  size_t h = hash(e->key) % BUCKETS;
  Entry **p = &db[h];
  while (*p && *p != e)
    p = &(*p)->next;
  if (*p) {
    *p = e->next;
    frees(e->key);
    freeobj(e->o);
    dbsize--;
    if (e->slot < dbsize) {
      keyslots[e->slot] = keyslots[dbsize];
      keyslots[e->slot]->slot = e->slot;
    }
    if (!dbsize) {
      xf(keyslots, keycapacity * sizeof *keyslots);
      keyslots = NULL;
      keycapacity = 0;
    }
    xf(e, sizeof *e);
  }
}

Entry *lookup(Str k) {
  Entry *e = db[hash(k) % BUCKETS];
  for (; e; e = e->next)
    if (eq(e->key, k)) {
      if (e->expires <= nowms()) {
        delentry(e);
        return NULL;
      }
      e->access = nowms();
      return e;
    }
  return NULL;
}

Entry *create(Str k, int type) {
  Entry *e = xm(sizeof *e);
  e->key = dupstr(k.p, k.n);
  e->o = newobj(type);
  e->expires = nowms() + DEFAULT_TTL;
  e->access = nowms();
  size_t h = hash(k) % BUCKETS;
  e->next = db[h];
  db[h] = e;
  if (dbsize == keycapacity) {
    size_t capacity = keycapacity ? keycapacity * 2 : 1;
    Entry **newindex = xm(capacity * sizeof *newindex);
    if (dbsize)
      memcpy(newindex, keyslots, dbsize * sizeof *newindex);
    xf(keyslots, keycapacity * sizeof *keyslots);
    keyslots = newindex;
    keycapacity = capacity;
  }
  e->slot = dbsize;
  keyslots[dbsize++] = e;
  return e;
}

void flush(void) {
  for (size_t h = 0; h < BUCKETS; h++)
    while (db[h])
      delentry(db[h]);
}
