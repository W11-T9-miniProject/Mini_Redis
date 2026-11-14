#include "server.h"
int before(Item *a, double score, Str k) {
  return a->score < score || (a->score == score && cmp(a->k, k) < 0);
}

void zinsert(Obj *o, Item *i) {
  Item *update[LEVELS];
  Item *x = NULL;
  for (int l = LEVELS - 1; l >= 0; l--) {
    Item *n = x ? x->forward[l] : o->skip[l];
    while (n && before(n, i->score, i->k)) {
      x = n;
      n = x->forward[l];
    }
    update[l] = x;
  }
  i->level = 1;
  while (i->level < LEVELS && (rand() & 3) == 0)
    i->level++;
  for (int l = 0; l < i->level; l++) {
    i->forward[l] = update[l] ? update[l]->forward[l] : o->skip[l];
    if (update[l])
      update[l]->forward[l] = i;
    else
      o->skip[l] = i;
  }
}

void zunlink(Obj *o, Item *i) {
  Item *x = NULL;
  for (int l = LEVELS - 1; l >= 0; l--) {
    Item *n = x ? x->forward[l] : o->skip[l];
    while (n && before(n, i->score, i->k)) {
      x = n;
      n = x->forward[l];
    }
    if (n == i) {
      if (x)
        x->forward[l] = i->forward[l];
      else
        o->skip[l] = i->forward[l];
    }
  }
  memset(i->forward, 0, sizeof i->forward);
}
