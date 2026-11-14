#include "server.h"
Obj *newobj(int type) {
  Obj *o = xm(sizeof *o);
  memset(o, 0, sizeof *o);
  o->type = type;
  return o;
}

void freeitem(Item *i) {
  frees(i->k);
  if (i->v.p)
    frees(i->v);
  xf(i, sizeof *i);
}

void freeobj(Obj *o) {
  if (o->type == STRING)
    frees(o->str);
  else {
    Item *i = o->head;
    while (i) {
      Item *n = i->next;
      freeitem(i);
      i = n;
    }
  }
  xf(o, sizeof *o);
}

Item *finditem(Obj *o, Str k) {
  for (Item *i = o->buckets[hash(k) % CBUCKETS]; i; i = i->hnext)
    if (eq(i->k, k))
      return i;
  return NULL;
}

Item *additem(Obj *o, Str k, Str *v) {
  Item *i = xm(sizeof *i);
  memset(i, 0, sizeof *i);
  i->k = dupstr(k.p, k.n);
  if (v)
    i->v = dupstr(v->p, v->n);
  i->prev = o->tail;
  if (o->tail)
    o->tail->next = i;
  else
    o->head = i;
  o->tail = i;
  size_t h = hash(k) % CBUCKETS;
  i->hnext = o->buckets[h];
  o->buckets[h] = i;
  o->count++;
  return i;
}

void removeitem(Obj *o, Item *i) {
  if (o->type == ZSET)
    zunlink(o, i);
  size_t h = hash(i->k) % CBUCKETS;
  Item **p = &o->buckets[h];
  while (*p != i)
    p = &(*p)->hnext;
  *p = i->hnext;
  if (i->prev)
    i->prev->next = i->next;
  else
    o->head = i->next;
  if (i->next)
    i->next->prev = i->prev;
  else
    o->tail = i->prev;
  o->count--;
  freeitem(i);
}
