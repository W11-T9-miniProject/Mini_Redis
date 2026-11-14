#ifndef MINI_REDIS_HASH_TABLE_H
#define MINI_REDIS_HASH_TABLE_H

#include "mini_redis/common.h"

typedef struct HtEntry {
    unsigned char *key;
    size_t key_len;
    unsigned char *value;
    size_t value_len;
    /* -1: 만료 없음. 그 외: 단조 증가 시계 기준 절대 ms. ht는 해석하지 않는다. */
    int64_t expires_at_ms;
    struct HtEntry *next;
    struct HtEntry *bucket_prev;
    /* 같은 노드를 두 가지 순서로 연결한다.
     * next/bucket_prev: 키 검색용 버킷 체인.
     * scan_next/scan_prev: 버킷 재배치와 무관한 만료 검사 순환 목록.
     * 덕분에 긴 충돌 체인도 다음 호출에서 이어 검사할 수 있다. */
    struct HtEntry *scan_next;
    struct HtEntry *scan_prev;
} HtEntry;

typedef struct {
    HtEntry **buckets;
    size_t capacity;
    size_t size;
    HtEntry *scan_cursor;
} HashTable;

/* 모든 init 실패 뒤에도 destroy가 안전하도록 출력은 먼저 0으로 초기화한다. */
MrStatus ht_init(HashTable *table, size_t initial_capacity);
void ht_destroy(HashTable *table);
uint64_t ht_hash(MrSlice key);

/* ht_find 결과는 빌린 포인터. 삭제/덮어쓰기/재배치 이후 보유하지 않는다. */
HtEntry *ht_find(HashTable *table, MrSlice key);

/* key/value를 복사해 소유한다. 실패하면 기존 값/TTL을 보존한다. */
MrStatus ht_set(HashTable *table, MrSlice key, MrSlice value, int64_t expires_at_ms);
MrStatus ht_delete(HashTable *table, MrSlice key);
/* 이미 찾은 노드를 다시 검색하지 않고 제거. store_sweep에서도 사용한다. */
void ht_remove_entry(HashTable *table, HtEntry *entry);
/* 기존 키를 새 버킷으로 재배치. 할당 실패 시 기존 테이블 보존. */
MrStatus ht_resize(HashTable *table, size_t new_capacity);

#endif
