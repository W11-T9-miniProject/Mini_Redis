#include "server.h"
Entry *db[BUCKETS];
size_t dbsize;

void expirecycle(void) {
  static size_t cursor;
  int64_t now = nowms();
  for (int n = 0; n < 256; n++) {
    size_t h = cursor++ % BUCKETS;
    Entry *e = db[h];
    while (e) {
      Entry *next = e->next;
      if (e->expires <= now)
        delentry(e);
      e = next;
    }
  }
}
void evict(void) {
  while (used_memory > maxmemory && dbsize) {
    Entry *victim = NULL;
    for (int n = 0; n < 5; n++) {
      Entry *e = keyslots[(size_t)rand() % dbsize];
      if (e && (!victim || e->access < victim->access))
        victim = e;
    }
    if (!victim)
      break;
    delentry(victim);
  }
}
