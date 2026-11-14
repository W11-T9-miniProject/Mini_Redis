#include "server.h"
int arity(Buf *out, int ok) {
  if (!ok)
    err(out, "wrong number of arguments");
  return ok;
}
int validttl(Str s, int64_t *v, Buf *b, int positive) {
  if (!integer(s, v)) {
    err(b, "value is not an integer or out of range");
    return 0;
  }
  if (*v > 300 || (positive && *v < 1)) {
    err(b, "invalid expire time (maximum 300 seconds)");
    return 0;
  }
  return 1;
}
void execute(int n, Str *a, Buf *out);
void evalcmd(int n, Str *a, Buf *out);
int gettype(Entry *e, int type, Buf *out) {
  if (e && e->o->type != type) {
    wrong(out);
    return 0;
  }
  return 1;
}
void execute(int n, Str *a, Buf *out) {
  if (!n) {
    err(out, "empty command");
    return;
  }
  if (word(a[0], "PING")) {
    if (n == 1)
      raw(out, "+PONG\r\n");
    else if (n == 2)
      bulk(out, a[1]);
    else
      arity(out, 0);
    return;
  }
  if (word(a[0], "EVAL")) {
    evalcmd(n, a, out);
    return;
  }
  if (word(a[0], "FLUSHALL")) {
    if (!arity(out, n == 1))
      return;
    flush();
    raw(out, "+OK\r\n");
    return;
  }
  if (word(a[0], "KEYS")) {
    if (!arity(out, n == 2))
      return;
    if (!word(a[1], "*")) {
      err(out, "only KEYS * is supported");
      return;
    }
    Buf values = {0};
    size_t count = 0;
    int64_t now = nowms();
    for (size_t h = 0; h < BUCKETS; h++) {
      Entry *e = db[h];
      while (e) {
        Entry *next = e->next;
        if (e->expires <= now)
          delentry(e);
        else {
          bulk(&values, e->key);
          count++;
        }
        e = next;
      }
    }
    num(out, '*', count);
    append(out, values.p, values.n);
    bfree(&values);
    return;
  }
  if (word(a[0], "DEL")) {
    if (!arity(out, n >= 2))
      return;
    int count = 0;
    for (int j = 1; j < n; j++) {
      Entry *e = lookup(a[j]);
      if (e) {
        delentry(e);
        count++;
      }
    }
    num(out, ':', count);
    return;
  }
  if (word(a[0], "SET")) {
    if (!arity(out, n == 5 || n == 6))
      return;
    int nx = 0, ex = 0;
    int64_t ttl = 0;
    for (int j = 3; j < n; j++) {
      if (word(a[j], "NX") && !nx)
        nx = 1;
      else if (word(a[j], "EX") && !ex && j + 1 < n) {
        ex = 1;
        if (!validttl(a[++j], &ttl, out, 1))
          return;
      } else {
        err(out, "syntax error");
        return;
      }
    }
    if (!ex) {
      err(out, "SET requires EX seconds");
      return;
    }
    Entry *e = lookup(a[1]);
    if (e && nx) {
      null(out);
      return;
    }
    if (e)
      delentry(e);
    e = create(a[1], STRING);
    e->o->str = dupstr(a[2].p, a[2].n);
    e->expires = nowms() + ttl * 1000;
    raw(out, "+OK\r\n");
    evict();
    return;
  }
  if (n < 2) {
    err(out, "wrong number of arguments");
    return;
  }
  Entry *e = lookup(a[1]);
  Obj *o = e ? e->o : NULL;
  if (word(a[0], "GET")) {
    if (!arity(out, n == 2) || !gettype(e, STRING, out))
      return;
    if (e)
      bulk(out, o->str);
    else
      null(out);
    return;
  }
  if (word(a[0], "TTL")) {
    if (!arity(out, n == 2))
      return;
    num(out, ':', e ? (e->expires - nowms() + 500) / 1000 : -2);
    return;
  }
  if (word(a[0], "EXPIRE")) {
    int64_t ttl;
    if (!arity(out, n == 3) || !validttl(a[2], &ttl, out, 1))
      return;
    if (e) {
      if (ttl <= 0)
        delentry(e);
      else
        e->expires = nowms() + ttl * 1000;
    }
    num(out, ':', e ? 1 : 0);
    return;
  }
  int type = 0;
  if (word(a[0], "RPUSH") || word(a[0], "LPOP") || word(a[0], "LRANGE") ||
      word(a[0], "LLEN"))
    type = LIST;
  else if (word(a[0], "SADD") || word(a[0], "SREM") ||
           word(a[0], "SISMEMBER") || word(a[0], "SMEMBERS"))
    type = SET;
  else if (word(a[0], "HSET") || word(a[0], "HGET") || word(a[0], "HDEL") ||
           word(a[0], "HGETALL"))
    type = HASH;
  else if (word(a[0], "ZADD") || word(a[0], "ZREM") || word(a[0], "ZCARD") ||
           word(a[0], "ZRANK"))
    type = ZSET;
  if (!type) {
    err(out, "unknown command");
    return;
  }
  if (!gettype(e, type, out))
    return;
  int write = 0;
  if (word(a[0], "RPUSH")) {
    if (!arity(out, n >= 3))
      return;
    if (!e) {
      e = create(a[1], LIST);
      o = e->o;
    }
    for (int j = 2; j < n; j++)
      additem(o, a[j], NULL);
    num(out, ':', o->count);
    write = 1;
  } else if (word(a[0], "LPOP")) {
    if (!arity(out, n == 2))
      return;
    if (!o || !o->head)
      null(out);
    else {
      bulk(out, o->head->k);
      removeitem(o, o->head);
    }
    write = 1;
  } else if (word(a[0], "LLEN") || word(a[0], "ZCARD")) {
    if (!arity(out, n == 2))
      return;
    num(out, ':', o ? o->count : 0);
  } else if (word(a[0], "LRANGE")) {
    int64_t start, end;
    if (!arity(out, n == 4))
      return;
    if (!integer(a[2], &start) || !integer(a[3], &end)) {
      err(out, "value is not an integer or out of range");
      return;
    }
    int64_t len = o ? (int64_t)o->count : 0;
    if (start < 0)
      start = start < -len ? 0 : start + len;
    if (end < 0)
      end = end < -len ? -1 : end + len;
    if (start < 0)
      start = 0;
    if (end >= len)
      end = len - 1;
    int64_t count = start > end ? 0 : end - start + 1;
    num(out, '*', count);
    Item *i = o ? o->head : NULL;
    for (int64_t j = 0; i && j <= end; j++, i = i->next)
      if (j >= start)
        bulk(out, i->k);
  } else if (word(a[0], "SADD")) {
    if (!arity(out, n >= 3))
      return;
    if (!e) {
      e = create(a[1], SET);
      o = e->o;
    }
    int count = 0;
    for (int j = 2; j < n; j++)
      if (!finditem(o, a[j])) {
        additem(o, a[j], NULL);
        count++;
      }
    num(out, ':', count);
    write = 1;
  } else if (word(a[0], "SREM") || word(a[0], "HDEL") || word(a[0], "ZREM")) {
    if (!arity(out, n >= 3))
      return;
    int count = 0;
    for (int j = 2; o && j < n; j++) {
      Item *i = finditem(o, a[j]);
      if (i) {
        removeitem(o, i);
        count++;
      }
    }
    num(out, ':', count);
    write = 1;
  } else if (word(a[0], "SISMEMBER")) {
    if (!arity(out, n == 3))
      return;
    num(out, ':', o && finditem(o, a[2]) ? 1 : 0);
  } else if (word(a[0], "SMEMBERS") || word(a[0], "HGETALL")) {
    if (!arity(out, n == 2))
      return;
    num(out, '*', o ? o->count * (type == HASH ? 2 : 1) : 0);
    for (Item *i = o ? o->head : NULL; i; i = i->next) {
      bulk(out, i->k);
      if (type == HASH)
        bulk(out, i->v);
    }
  } else if (word(a[0], "HGET")) {
    if (!arity(out, n == 3))
      return;
    Item *i = o ? finditem(o, a[2]) : NULL;
    if (i)
      bulk(out, i->v);
    else
      null(out);
  } else if (word(a[0], "HSET")) {
    if (!arity(out, n >= 4 && n % 2 == 0))
      return;
    if (!e) {
      e = create(a[1], HASH);
      o = e->o;
    }
    int count = 0;
    for (int j = 2; j < n; j += 2) {
      Item *i = finditem(o, a[j]);
      if (i) {
        frees(i->v);
        i->v = dupstr(a[j + 1].p, a[j + 1].n);
      } else {
        additem(o, a[j], &a[j + 1]);
        count++;
      }
    }
    num(out, ':', count);
    write = 1;
  } else if (word(a[0], "ZADD")) {
    if (!arity(out, n >= 4 && n % 2 == 0))
      return;
    double scores[MAXARG / 2];
    for (int j = 2; j < n; j += 2)
      if (!realnum(a[j], &scores[j / 2])) {
        err(out, "value is not a valid float");
        return;
      }
    if (!e) {
      e = create(a[1], ZSET);
      o = e->o;
    }
    int count = 0;
    for (int j = 2; j < n; j += 2) {
      Item *i = finditem(o, a[j + 1]);
      if (i)
        zunlink(o, i);
      else {
        i = additem(o, a[j + 1], NULL);
        count++;
      }
      i->score = scores[j / 2];
      zinsert(o, i);
    }
    num(out, ':', count);
    write = 1;
  } else if (word(a[0], "ZRANK")) {
    if (!arity(out, n == 3))
      return;
    Item *t = o ? finditem(o, a[2]) : NULL;
    if (!t)
      null(out);
    else {
      int64_t rank = 0;
      for (Item *i = o->skip[0]; i && i != t; i = i->forward[0])
        rank++;
      num(out, ':', rank);
    }
  }
  if (write) {
    if (e && o->count == 0)
      delentry(e);
    evict();
  }
}
