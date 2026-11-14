#include "server.h"
/* Redis RDB v9, uncompressed canonical encodings; zero checksum is Redis's
 * disabled-checksum form. */
char datadir[PATH_MAX] = ".";
pid_t snapshot_pid = -1;
int snapshot_interval = 60;
int wr(FILE *f, const void *p, size_t n) { return fwrite(p, 1, n, f) == n; }
int byte(FILE *f, unsigned char x) { return wr(f, &x, 1); }
int lengthwrite(FILE *f, uint64_t n) {
  if (n < 64)
    return byte(f, (unsigned char)n);
  if (n < 16384)
    return byte(f, (unsigned char)((n >> 8) | 0x40)) &&
           byte(f, (unsigned char)n);
  if (n <= UINT32_MAX) {
    uint32_t v = htonl((uint32_t)n);
    return byte(f, 0x80) && wr(f, &v, 4);
  }
  unsigned char a[9] = {0x81};
  for (int j = 0; j < 8; j++)
    a[j + 1] = (unsigned char)(n >> (56 - 8 * j));
  return wr(f, a, 9);
}
int stringwrite(FILE *f, Str s) {
  return lengthwrite(f, s.n) && wr(f, s.p, s.n);
}
int littlewrite(FILE *f, uint64_t n) {
  unsigned char a[8];
  for (int j = 0; j < 8; j++)
    a[j] = (unsigned char)(n >> (8 * j));
  return wr(f, a, 8);
}
static int filecrc(FILE *f, long length, uint64_t *out) {
  static uint64_t table[256];
  static int initialized;
  if (!initialized) {
    for (unsigned i = 0; i < 256; i++) {
      uint64_t crc = i;
      for (int bit = 0; bit < 8; bit++)
        crc = (crc >> 1) ^ ((crc & 1) ? UINT64_C(0x95ac9329ac4bc9b5) : 0);
      table[i] = crc;
    }
    initialized = 1;
  }
  if (fseek(f, 0, SEEK_SET))
    return 0;
  uint64_t crc = 0;
  unsigned char buf[8192];
  while (length > 0) {
    size_t wanted = length > (long)sizeof buf ? sizeof buf : (size_t)length;
    if (fread(buf, 1, wanted, f) != wanted)
      return 0;
    for (size_t j = 0; j < wanted; j++)
      crc = table[(crc ^ buf[j]) & 255] ^ (crc >> 8);
    length -= (long)wanted;
  }
  *out = crc;
  return 1;
}
int savefile(void) {
  char temp[PATH_MAX + 64], dest[PATH_MAX + 32], archive[PATH_MAX + 64];
  snprintf(temp, sizeof temp, "%s/.dump-%ld.tmp", datadir, (long)getpid());
  snprintf(dest, sizeof dest, "%s/dump.rdb", datadir);
  FILE *f = fopen(temp, "w+b");
  if (!f)
    return 0;
  int ok = wr(f, "REDIS0009", 9) && byte(f, 254) && lengthwrite(f, 0);
  int64_t now = nowms();
  for (size_t h = 0; ok && h < BUCKETS; h++)
    for (Entry *e = db[h]; ok && e; e = e->next) {
      if (e->expires <= now)
        continue;
      Obj *o = e->o;
      int type = o->type == STRING ? 0
                 : o->type == LIST ? 1
                 : o->type == SET  ? 2
                 : o->type == HASH ? 4
                                   : 5;
      ok = byte(f, 252) && littlewrite(f, (uint64_t)e->expires) &&
           byte(f, (unsigned char)type) && stringwrite(f, e->key);
      if (o->type == STRING)
        ok = ok && stringwrite(f, o->str);
      else {
        ok = ok && lengthwrite(f, o->count);
        for (Item *i = o->head; ok && i; i = i->next) {
          ok = stringwrite(f, i->k);
          if (o->type == HASH)
            ok = ok && stringwrite(f, i->v);
          if (o->type == ZSET) {
            uint64_t bits;
            memcpy(&bits, &i->score, 8);
            ok = ok && littlewrite(f, bits);
          }
        }
      }
    }
  ok = ok && byte(f, 255);
  long checksum_end = ftell(f);
  uint64_t checksum = 0;
  if (checksum_end < 0 || fflush(f) || !filecrc(f, checksum_end, &checksum) ||
      fseek(f, 0, SEEK_END))
    ok = 0;
  ok = ok && littlewrite(f, checksum);
  if (fflush(f) || fsync(fileno(f)))
    ok = 0;
  if (fclose(f))
    ok = 0;
  if (ok && rename(temp, dest))
    ok = 0;
  if (!ok) {
    unlink(temp);
    return 0;
  }
  snprintf(archive, sizeof archive, "%s/snapshot-%lld.rdb", datadir,
           (long long)now);
  if (link(dest, archive) && errno != EEXIST)
    fprintf(stderr, "snapshot archive: %s\n", strerror(errno));
  int dirfd = open(datadir, O_RDONLY);
  if (dirfd >= 0) {
    fsync(dirfd);
    close(dirfd);
  }
  DIR *d = opendir(datadir);
  if (d) {
    struct dirent *ent;
    while ((ent = readdir(d))) {
      long long stamp;
      char extra;
      if (sscanf(ent->d_name, "snapshot-%lld.rdb%c", &stamp, &extra) == 1 &&
          stamp < now - 86400000) {
        char path[PATH_MAX + 512];
        snprintf(path, sizeof path, "%s/%s", datadir, ent->d_name);
        unlink(path);
      }
    }
    closedir(d);
  }
  return 1;
}
int rd(FILE *f, void *p, size_t n) { return fread(p, 1, n, f) == n; }
int lengthread(FILE *f, uint64_t *n) {
  int c = fgetc(f);
  if (c < 0)
    return 0;
  if ((c >> 6) == 0) {
    *n = (unsigned)c;
    return 1;
  }
  if ((c >> 6) == 1) {
    int d = fgetc(f);
    if (d < 0)
      return 0;
    *n = ((unsigned)(c & 63) << 8) | (unsigned)d;
    return 1;
  }
  if (c == 128) {
    uint32_t x;
    if (!rd(f, &x, 4))
      return 0;
    *n = ntohl(x);
    return 1;
  }
  if (c == 129) {
    unsigned char b[8];
    if (!rd(f, b, 8))
      return 0;
    *n = 0;
    for (int j = 0; j < 8; j++)
      *n = (*n << 8) | b[j];
    return 1;
  }
  return 0;
}
int stringread(FILE *f, Str *s) {
  uint64_t n;
  if (!lengthread(f, &n) || n > MAXFRAME)
    return 0;
  *s = dupstr("", 0);
  frees(*s);
  s->n = (size_t)n;
  s->p = xm((size_t)n + 1);
  s->p[n] = 0;
  if (!rd(f, s->p, (size_t)n)) {
    frees(*s);
    memset(s, 0, sizeof *s);
    return 0;
  }
  return 1;
}
int littleread(FILE *f, uint64_t *v) {
  unsigned char b[8];
  if (!rd(f, b, 8))
    return 0;
  *v = 0;
  for (int j = 0; j < 8; j++)
    *v |= (uint64_t)b[j] << (j * 8);
  return 1;
}
int loadfile(void) {
  char path[PATH_MAX + 32];
  snprintf(path, sizeof path, "%s/dump.rdb", datadir);
  FILE *f = fopen(path, "rb");
  if (!f)
    return errno == ENOENT;
  char header[9];
  int ok = rd(f, header, 9) && !memcmp(header, "REDIS0009", 9);
  uint64_t expiry = 0;
  int ended = 0;
  while (ok) {
    int t = fgetc(f);
    if (t < 0) {
      ok = 0;
      break;
    }
    if (t == 255) {
      uint64_t checksum;
      long checksum_end = ftell(f);
      ok = littleread(f, &checksum) && fgetc(f) == EOF;
      if (ok && checksum) {
        uint64_t actual = 0;
        ok = filecrc(f, checksum_end, &actual) && actual == checksum;
      }
      ended = 1;
      break;
    }
    if (t == 254) {
      uint64_t index;
      ok = lengthread(f, &index) && index == 0;
      continue;
    }
    if (t == 252) {
      ok = littleread(f, &expiry);
      continue;
    }
    int type = t == 0   ? STRING
               : t == 1 ? LIST
               : t == 2 ? SET
               : t == 4 ? HASH
               : t == 5 ? ZSET
                        : 0;
    if (!type || !expiry) {
      ok = 0;
      break;
    }
    Str key = {0};
    if (!stringread(f, &key)) {
      ok = 0;
      break;
    }
    Entry *existing = lookup(key);
    if (existing)
      delentry(existing);
    Entry *e = create(key, type);
    frees(key);
    e->expires = (int64_t)expiry;
    expiry = 0;
    Obj *o = e->o;
    if (type == STRING) {
      ok = stringread(f, &o->str);
    } else {
      uint64_t count;
      ok = lengthread(f, &count) && count > 0 && count <= 10000000;
      for (uint64_t j = 0; ok && j < count; j++) {
        Str k = {0}, v = {0};
        ok = stringread(f, &k);
        if (ok && type == HASH)
          ok = stringread(f, &v);
        if (ok) {
          Item *i = additem(o, k, type == HASH ? &v : NULL);
          if (type == ZSET) {
            uint64_t bits;
            ok = littleread(f, &bits);
            if (ok) {
              memcpy(&i->score, &bits, 8);
              if (isnan(i->score))
                ok = 0;
              else
                zinsert(o, i);
            }
          }
        }
        if (k.p)
          frees(k);
        if (v.p)
          frees(v);
      }
    }
    if (e->expires <= nowms())
      delentry(e);
  }
  fclose(f);
  if (!ok || !ended) {
    flush();
    fprintf(stderr, "Invalid or unsupported RDB: %s\n", path);
    return 0;
  }
  evict();
  return 1;
}
void backgroundsnapshot(void) {
  if (snapshot_pid > 0)
    return;
  pid_t p = fork();
  if (p == 0) {
    int ok = savefile();
    _exit(ok ? 0 : 1);
  }
  if (p > 0)
    snapshot_pid = p;
  else
    perror("fork snapshot");
}
