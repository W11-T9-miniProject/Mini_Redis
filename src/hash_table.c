#include "mini_redis/hash_table.h"

#include <stdlib.h>
#include <string.h>

/* 이 계층은 TTL을 판단하지 않는다. 버킷 위치, 충돌 연결, 메모리 소유만 담당.
 * command_execute → store_* → ht_* 순서로 호출된다. */
static bool slice_valid(MrSlice slice) {
    return slice.len == 0 || slice.data != NULL;
}

MrStatus ht_init(HashTable *table, size_t initial_capacity) {
    if (!table) return MR_INVALID;
    memset(table, 0, sizeof(*table));
    if (!initial_capacity || initial_capacity > SIZE_MAX / sizeof(HtEntry *)) return MR_INVALID;
    table->buckets = mr_calloc(initial_capacity, sizeof(HtEntry *));
    if (!table->buckets) return MR_OOM;
    table->capacity = initial_capacity;
    return MR_OK;
}

uint64_t ht_hash(MrSlice key) {
    /* FNV-1a: 각 바이트를 xor하고 소수를 곱한다. unsigned wraparound는 의도적.
     * 학습용 비암호학적 해시다. 공격자가 구성한 충돌을 방어하는 해시는 아니다.
     * strlen 대신 len을 사용해서 중간 NUL도 키의 일부로 처리한다. */
    uint64_t hash = UINT64_C(14695981039346656037);
    for (size_t i = 0; i < key.len; ++i) {
        hash ^= key.data[i];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

HtEntry *ht_find(HashTable *table, MrSlice key) {
    if (!table || !table->capacity || !slice_valid(key)) return NULL;
    size_t index = (size_t)(ht_hash(key) % table->capacity);
    for (HtEntry *entry = table->buckets[index]; entry; entry = entry->next) {
        if (mr_slice_equal((MrSlice){entry->key, entry->key_len}, key)) return entry;
    }
    return NULL;
}

MrStatus ht_resize(HashTable *table, size_t new_capacity) {
    if (!table || !table->capacity || !new_capacity ||
        new_capacity > SIZE_MAX / sizeof(HtEntry *)) return MR_INVALID;
    HtEntry **buckets = mr_calloc(new_capacity, sizeof(HtEntry *));
    if (!buckets) return MR_OOM;
    /* 할당 성공 뒤에만 연결을 변경. 노드/키/값은 복사하지 않고 버킷만 바꾼다.
     * 별도의 scan 연결은 그대로여서 만료 정리 도중 확장해도 순회가 이어진다. */
    for (size_t i = 0; i < table->capacity; ++i) {
        HtEntry *entry = table->buckets[i];
        while (entry) {
            HtEntry *next = entry->next;
            size_t index = (size_t)(ht_hash((MrSlice){entry->key, entry->key_len}) % new_capacity);
            entry->bucket_prev = NULL;
            entry->next = buckets[index];
            if (entry->next) entry->next->bucket_prev = entry;
            buckets[index] = entry;
            entry = next;
        }
    }
    free(table->buckets);
    table->buckets = buckets;
    table->capacity = new_capacity;
    return MR_OK;
}

MrStatus ht_set(HashTable *table, MrSlice key, MrSlice value, int64_t expires_at_ms) {
    if (!table || !table->capacity || !slice_valid(key) || !slice_valid(value) || expires_at_ms < -1)
        return MR_INVALID;
    HtEntry *existing = ht_find(table, key);
    /* 입력이 네트워크 버퍼 또는 기존 노드 메모리일 수 있으므로 먼저 복사한다.
     * 이후 원본 버퍼가 변경/해제되어도 저장된 데이터는 독립적으로 살아 있다. */
    unsigned char *copy = mr_malloc(value.len);
    if (!copy) return MR_OOM;
    if (value.len) memcpy(copy, value.data, value.len);
    if (existing) {
        free(existing->value);
        existing->value = copy;
        existing->value_len = value.len;
        existing->expires_at_ms = expires_at_ms;
        return MR_OK;
    }
    HtEntry *entry = mr_calloc(1, sizeof(*entry));
    if (!entry) { free(copy); return MR_OOM; }
    entry->key = mr_malloc(key.len);
    if (!entry->key) { free(entry); free(copy); return MR_OOM; }
    if (key.len) memcpy(entry->key, key.data, key.len);
    entry->key_len = key.len;
    entry->value = copy;
    entry->value_len = value.len;
    entry->expires_at_ms = expires_at_ms;

    /* 새 노드의 모든 할당을 끝낸 다음 확장한다. 실패하면 기존 키/값/TTL 보존.
     * capacity - capacity/4는 약 75% 적재율 기준이며 곱셈 overflow를 피한다. */
    MrStatus status = MR_OK;
    if (table->size == SIZE_MAX) status = MR_LIMIT;
    else if (table->size + 1 > table->capacity - table->capacity / 4) {
        if (table->capacity > SIZE_MAX / 2) status = MR_LIMIT;
        else status = ht_resize(table, table->capacity * 2);
    }
    if (status != MR_OK) {
        free(entry->key); free(entry->value); free(entry);
        return status;
    }

    size_t index = (size_t)(ht_hash(key) % table->capacity);
    entry->next = table->buckets[index];
    if (entry->next) entry->next->bucket_prev = entry;
    table->buckets[index] = entry;
    /* 검색 체인 외에 모든 노드를 잇는 순환 목록에도 넣는다.
     * 순환 목록은 긴 충돌 체인의 뒷부분까지 예산 내에서 이어 검사하기 위해 필요. */
    if (!table->scan_cursor) {
        entry->scan_next = entry->scan_prev = entry;
        table->scan_cursor = entry;
    } else {
        HtEntry *cursor = table->scan_cursor;
        entry->scan_prev = cursor->scan_prev;
        entry->scan_next = cursor;
        cursor->scan_prev->scan_next = entry;
        cursor->scan_prev = entry;
    }
    ++table->size;
    return MR_OK;
}

void ht_remove_entry(HashTable *table, HtEntry *entry) {
    /* entry는 이 테이블의 살아 있는 노드여야 한다.
     * 검색 체인과 순회 체인에서 먼저 분리하고, 마지막에 실제 메모리를 해제한다. */
    if (entry->bucket_prev) entry->bucket_prev->next = entry->next;
    else {
        size_t index = (size_t)(ht_hash((MrSlice){entry->key, entry->key_len}) % table->capacity);
        table->buckets[index] = entry->next;
    }
    if (entry->next) entry->next->bucket_prev = entry->bucket_prev;
    if (entry->scan_next == entry) table->scan_cursor = NULL;
    else {
        if (table->scan_cursor == entry) table->scan_cursor = entry->scan_next;
        entry->scan_prev->scan_next = entry->scan_next;
        entry->scan_next->scan_prev = entry->scan_prev;
    }
    free(entry->key);
    free(entry->value);
    free(entry);
    --table->size;
}

MrStatus ht_delete(HashTable *table, MrSlice key) {
    if (!table || !table->capacity || !slice_valid(key)) return MR_INVALID;
    HtEntry *entry = ht_find(table, key);
    if (!entry) return MR_NOT_FOUND;
    ht_remove_entry(table, entry);
    return MR_OK;
}

void ht_destroy(HashTable *table) {
    if (!table) return;
    while (table->scan_cursor) ht_remove_entry(table, table->scan_cursor);
    free(table->buckets);
    memset(table, 0, sizeof(*table));
}
