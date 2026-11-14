#include "mini_redis/store.h"

#include <string.h>

static bool valid(Store *store, MrSlice key, int64_t now_ms) {
    return store && store->table.capacity && (key.len == 0 || key.data) && now_ms >= 0;
}

static bool expired(const HtEntry *entry, int64_t now_ms) {
    return entry->expires_at_ms != -1 && now_ms >= entry->expires_at_ms;
}

/* 모든 명령에서 '존재'를 동일하게 해석하기 위한 공통 진입점.
 * 노드가 물리적으로 있어도 TTL이 지났으면 제거하고 없음으로 반환한다. */
static HtEntry *find_live(Store *store, MrSlice key, int64_t now_ms) {
    HtEntry *entry = ht_find(&store->table, key);
    if (entry && expired(entry, now_ms)) {
        ht_remove_entry(&store->table, entry);
        return NULL;
    }
    return entry;
}

MrStatus store_init(Store *store, size_t capacity) {
    if (!store) return MR_INVALID;
    memset(store, 0, sizeof(*store));
    return ht_init(&store->table, capacity);
}

void store_destroy(Store *store) {
    if (!store) return;
    ht_destroy(&store->table);
    store->last_sweep_checked = 0;
}

MrStatus store_set(Store *store, MrSlice key, MrSlice value, int64_t ttl_ms, int64_t now_ms) {
    if (!valid(store, key, now_ms) || (value.len && !value.data) ||
        ttl_ms == 0 || ttl_ms < -1 || (ttl_ms > 0 && now_ms > INT64_MAX - ttl_ms)) return MR_INVALID;
    /* 값과 기한을 ht_set 한 번으로 바꾼다. 별도의 SET/EXPIRE 사이 틈이 없다.
     * -1은 무기한이며, 기존 TTL도 제거한다. 더하기 전에 overflow를 검사한다. */
    int64_t deadline = ttl_ms == -1 ? -1 : now_ms + ttl_ms;
    return ht_set(&store->table, key, value, deadline);
}

MrStatus store_get(Store *store, MrSlice key, int64_t now_ms, MrSlice *value) {
    if (!value) return MR_INVALID;
    *value = (MrSlice){NULL, 0};
    if (!valid(store, key, now_ms)) return MR_INVALID;
    HtEntry *entry = find_live(store, key, now_ms);
    if (!entry) return MR_NOT_FOUND;
    *value = (MrSlice){entry->value, entry->value_len};
    return MR_OK;
}

MrStatus store_delete(Store *store, MrSlice key, int64_t now_ms) {
    if (!valid(store, key, now_ms)) return MR_INVALID;
    HtEntry *entry = find_live(store, key, now_ms);
    if (!entry) return MR_NOT_FOUND;
    ht_remove_entry(&store->table, entry);
    return MR_OK;
}

MrStatus store_expire(Store *store, MrSlice key, int64_t ttl_ms, int64_t now_ms) {
    if (!valid(store, key, now_ms) || (ttl_ms > 0 && now_ms > INT64_MAX - ttl_ms)) return MR_INVALID;
    HtEntry *entry = find_live(store, key, now_ms);
    if (!entry) return MR_NOT_FOUND;
    if (ttl_ms <= 0) ht_remove_entry(&store->table, entry);
    else entry->expires_at_ms = now_ms + ttl_ms;
    return MR_OK;
}

MrStatus store_ttl(Store *store, MrSlice key, int64_t now_ms, int64_t *remaining_ms) {
    if (!remaining_ms) return MR_INVALID;
    *remaining_ms = -2;
    if (!valid(store, key, now_ms)) return MR_INVALID;
    HtEntry *entry = find_live(store, key, now_ms);
    if (!entry) return MR_NOT_FOUND;
    *remaining_ms = entry->expires_at_ms == -1 ? -1 : entry->expires_at_ms - now_ms;
    return MR_OK;
}

MrStatus store_sweep(Store *store, int64_t now_ms, size_t budget, size_t *deleted) {
    if (!deleted) return MR_INVALID;
    *deleted = 0;
    if (!valid(store, (MrSlice){NULL, 0}, now_ms)) return MR_INVALID;
    store->last_sweep_checked = 0;
    size_t checks = budget < store->table.size ? budget : store->table.size;
    /* cursor를 먼저 전진한 후 검사/삭제한다. ht_remove_entry가 마지막 노드와
     * cursor 삭제를 보정한다. 재해시에도 scan 연결은 변하지 않는다.
     * 한 호출에서 같은 노드를 여러 번 도는 대신 최대 한 바퀴까지만 검사한다. */
    for (size_t i = 0; i < checks; ++i) {
        HtEntry *entry = store->table.scan_cursor;
        store->table.scan_cursor = entry->scan_next;
        ++store->last_sweep_checked;
        if (expired(entry, now_ms)) {
            ht_remove_entry(&store->table, entry);
            ++*deleted;
        }
    }
    return MR_OK;
}
