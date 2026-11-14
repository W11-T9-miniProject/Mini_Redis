#include "server.h"
/* Sandboxed Lua: no standard libraries, explicit key capability, bounded
 * allocator. */

void *luaalloc(void *ud, void *ptr, size_t old, size_t n) {
  LuaCtx *c = ud;
  if (!ptr)
    old = 0;
  if (!n) {
    free(ptr);
    c->bytes -= old;
    return NULL;
  }
  if (n > 8 * 1024 * 1024 || c->bytes - old + n > 8 * 1024 * 1024)
    return NULL;
  void *p = realloc(ptr, n);
  if (p)
    c->bytes = c->bytes - old + n;
  return p;
}
LuaCtx *luactx(lua_State *L) { return *(LuaCtx **)lua_getextraspace(L); }
void luahook(lua_State *L, lua_Debug *d) {
  (void)d;
  LuaCtx *c = luactx(L);
  if (++c->expired > 1000 || nowms() > c->deadline)
    luaL_error(L, "script execution limit exceeded");
}
int readonly(lua_State *L) {
  return luaL_error(L, "table modification is not allowed");
}
int keyindex(lua_State *L) {
  LuaCtx *c = luactx(L);
  lua_Integer i = luaL_checkinteger(L, 2);
  int kind = (int)lua_tointeger(L, lua_upvalueindex(1));
  int n = kind ? c->nargs : c->nkeys;
  Str *a = kind ? c->args : c->keys;
  if (i < 1 || i > n)
    lua_pushnil(L);
  else
    lua_pushlstring(L, (char *)a[i - 1].p, a[i - 1].n);
  return 1;
}
int rediscall(lua_State *L) {
  LuaCtx *c = luactx(L);
  int safe = (int)lua_tointeger(L, lua_upvalueindex(1));
  int n = lua_gettop(L);
  const char *failure = NULL;
  Str a[MAXARG];
  Buf out = {0};
  if (n < 2 || n > MAXARG)
    failure = "invalid redis.call arguments";
  else if (++c->calls > 1000)
    failure = "script command limit exceeded";
  else {
    for (int j = 0; j < n; j++) {
      if (!lua_isstring(L, j + 1)) {
        failure = "redis command arguments must be strings or numbers";
        break;
      }
      size_t len;
      const char *p = lua_tolstring(L, j + 1, &len);
      a[j] = (Str){len, (unsigned char *)p};
    }
    if (!failure &&
        !(word(a[0], "GET") || word(a[0], "SET") || word(a[0], "DEL") ||
          word(a[0], "EXPIRE") || word(a[0], "TTL")))
      failure = "command is not allowed in scripts";
    if (!failure) {
      int last = word(a[0], "DEL") ? n : 2;
      for (int j = 1; j < last; j++) {
        int found = 0;
        for (int k = 0; k < c->nkeys; k++)
          if (eq(a[j], c->keys[k]))
            found = 1;
        if (!found) {
          failure = "script accessed an undeclared key";
          break;
        }
      }
    }
  }
  if (!failure) { /* RESP argument strings require a terminator for numeric
                     parsers. */
    Str copies[MAXARG];
    for (int j = 0; j < n; j++)
      copies[j] = dupstr(a[j].p, a[j].n);
    execute(n, copies, &out);
    for (int j = 0; j < n; j++)
      frees(copies[j]);
    c->scratch = out;
    if (out.n && out.p[0] == '-') {
      size_t len = out.n - 3;
      lua_pushlstring(L, (char *)out.p + 1, len);
      bfree(&c->scratch);
      if (safe)
        return 1;
      return lua_error(L);
    }
    if (out.n && out.p[0] == ':')
      lua_pushinteger(L, strtoll((char *)out.p + 1, NULL, 10));
    else if (out.n && out.p[0] == '+')
      lua_pushlstring(L, (char *)out.p + 1, out.n - 3);
    else if (out.n && out.p[0] == '$') {
      if (out.n >= 5 && !memcmp(out.p, "$-1", 3))
        lua_pushboolean(L, 0);
      else {
        unsigned char *p = (unsigned char *)memchr(out.p, '\n', out.n);
        size_t len = (size_t)strtoull((char *)out.p + 1, NULL, 10);
        lua_pushlstring(L, (char *)p + 1, len);
      }
    } else
      lua_pushnil(L);
    bfree(&c->scratch);
    return 1;
  }
  if (safe) {
    lua_pushstring(L, failure);
    return 1;
  }
  return luaL_error(L, "%s", failure);
}
/* Token screening is structural, and skips literals/comments rather than
 * searching source text. */
int scriptcheck(Str s) {
  size_t j = 0;
  while (j < s.n) {
    unsigned char c = s.p[j];
    if (c == '\'' || c == '"') {
      unsigned char quote = c;
      j++;
      int closed = 0;
      while (j < s.n) {
        if (s.p[j] == '\\') {
          j += 2;
          continue;
        }
        if (s.p[j++] == quote) {
          closed = 1;
          break;
        }
      }
      if (!closed)
        return 0;
      continue;
    }
    if (c == '-' && j + 1 < s.n && s.p[j + 1] == '-') {
      j += 2;
      if (j + 1 < s.n && s.p[j] == '[' && s.p[j + 1] == '[')
        return 0;
      while (j < s.n && s.p[j] != '\n')
        j++;
      continue;
    }
    if (c == '{' || c == '}' || c == ':' || c == ']')
      return 0;
    if (c == '[')
      return 0;
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_') {
      size_t begin = j++;
      while (j < s.n && ((s.p[j] >= 'a' && s.p[j] <= 'z') ||
                         (s.p[j] >= 'A' && s.p[j] <= 'Z') ||
                         (s.p[j] >= '0' && s.p[j] <= '9') || s.p[j] == '_'))
        j++;
      Str id = {j - begin, s.p + begin};
      if (word(id, "WHILE") || word(id, "REPEAT") || word(id, "UNTIL") ||
          word(id, "GOTO"))
        return 0;
      if (word(id, "KEYS") || word(id, "ARGV")) {
        while (j < s.n && (s.p[j] == ' ' || s.p[j] == '\t'))
          j++;
        if (j < s.n && s.p[j] == '[') {
          j++;
          while (j < s.n && s.p[j] != ']') {
            if (!(s.p[j] == ' ' || (s.p[j] >= '0' && s.p[j] <= '9') ||
                  (s.p[j] >= 'a' && s.p[j] <= 'z') || s.p[j] == '_' ||
                  s.p[j] == '+' || s.p[j] == '-'))
              return 0;
            j++;
          }
          if (j == s.n)
            return 0;
          j++;
        }
        continue;
      }
      if (word(id, "REDIS")) {
        if (j >= s.n || s.p[j++] != '.')
          return 0;
        size_t b = j;
        while (j < s.n && s.p[j] >= 'a' && s.p[j] <= 'z')
          j++;
        Str method = {j - b, s.p + b};
        if (!word(method, "CALL") && !word(method, "PCALL"))
          return 0;
        continue;
      }
      continue;
    }
    if (c == '.' && !(j + 1 < s.n && s.p[j + 1] >= '0' && s.p[j + 1] <= '9'))
      return 0;
    j++;
  }
  return 1;
}
void keyproxy(lua_State *L, const char *name, int kind) {
  lua_newtable(L);
  lua_newtable(L);
  lua_pushinteger(L, kind);
  lua_pushcclosure(L, keyindex, 1);
  lua_setfield(L, -2, "__index");
  lua_pushcfunction(L, readonly);
  lua_setfield(L, -2, "__newindex");
  lua_setmetatable(L, -2);
  lua_setglobal(L, name);
}
void evalcmd(int n, Str *a, Buf *out) {
  int64_t nk;
  if (!arity(out, n >= 3))
    return;
  if (!integer(a[2], &nk) || nk < 0 || nk > n - 3) {
    err(out, "invalid number of keys");
    return;
  }
  if (a[1].n > 65536 || !scriptcheck(a[1])) {
    err(out, "script uses forbidden syntax (tables are not allowed)");
    return;
  }
  LuaCtx c = {.nkeys = (int)nk,
              .nargs = n - 3 - (int)nk,
              .keys = a + 3,
              .args = a + 3 + nk,
              .deadline = nowms() + 50};
#if LUA_VERSION_NUM >= 505
  lua_State *L = lua_newstate(luaalloc, &c, (unsigned)time(NULL));
#else
  lua_State *L = lua_newstate(luaalloc, &c);
#endif
  if (!L) {
    err(out, "script allocation failed");
    return;
  }
  *(LuaCtx **)lua_getextraspace(L) = &c;
  lua_sethook(L, luahook, LUA_MASKCOUNT, 1000);
  keyproxy(L, "KEYS", 0);
  keyproxy(L, "ARGV", 1);
  lua_newtable(L);
  lua_pushinteger(L, 0);
  lua_pushcclosure(L, rediscall, 1);
  lua_setfield(L, -2, "call");
  lua_pushinteger(L, 1);
  lua_pushcclosure(L, rediscall, 1);
  lua_setfield(L, -2, "pcall");
  lua_setglobal(L, "redis");
  int status = luaL_loadbufferx(L, (char *)a[1].p, a[1].n, "script", "t");
  if (status == LUA_OK)
    status = lua_pcall(L, 0, 1, 0);
  if (status != LUA_OK) {
    const char *p = lua_tostring(L, -1);
    err(out, p ? p : "script failed");
  } else {
    int t = lua_type(L, -1);
    if (t == LUA_TNIL || (t == LUA_TBOOLEAN && !lua_toboolean(L, -1)))
      null(out);
    else if (t == LUA_TBOOLEAN)
      num(out, ':', 1);
    else if (t == LUA_TNUMBER) {
      lua_Number d = lua_tonumber(L, -1);
      if (!isfinite(d) || d >= (lua_Number)INT64_MAX ||
          d < (lua_Number)INT64_MIN)
        err(out, "script number out of range");
      else
        num(out, ':', (int64_t)d);
    } else if (t == LUA_TSTRING) {
      size_t len;
      const char *p = lua_tolstring(L, -1, &len);
      bulk(out, (Str){len, (unsigned char *)p});
    } else
      err(out, "unsupported script return type");
  }
  bfree(&c.scratch);
  lua_close(L);
  evict();
}
