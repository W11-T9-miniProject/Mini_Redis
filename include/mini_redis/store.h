#ifndef MINI_REDIS_STORE_H
#define MINI_REDIS_STORE_H

#include "mini_redis/hash_table.h"

typedef struct {
    HashTable table;
    size_t last_sweep_checked; /* 작업량 상한을 테스트/관찰하기 위한 값 */
} Store;

MrStatus store_init(Store *store, size_t capacity);
void store_destroy(Store *store);

/* ttl_ms=-1이면 무기한. SET의 0 또는 -1 미만 TTL은 MR_INVALID.
 * EXPIRE의 <=0 즉시 삭제 규칙과 구분한다. now_ms는 0 이상. */
MrStatus store_set(Store *store, MrSlice key, MrSlice value, int64_t ttl_ms, int64_t now_ms);
/* value는 내부 참조. 다음 저장소 변경 전에 응답 버퍼로 복사한다. */
MrStatus store_get(Store *store, MrSlice key, int64_t now_ms, MrSlice *value);
MrStatus store_delete(Store *store, MrSlice key, int64_t now_ms);
MrStatus store_expire(Store *store, MrSlice key, int64_t ttl_ms, int64_t now_ms);
/* MR_OK와 -1: 무기한, MR_NOT_FOUND와 -2: 키 없음. 그 외 남은 ms. */
MrStatus store_ttl(Store *store, MrSlice key, int64_t now_ms, int64_t *remaining_ms);

/* budget는 삭제 개수가 아닌 확인할 노드 수 상한.
 * 긴 충돌 체인에서도 한 번의 호출이 서버 전체를 오래 붙잡지 않게 설계한다.
 * 완료 후 실제 삭제 수를 출력한다. */
MrStatus store_sweep(Store *store, int64_t now_ms, size_t budget, size_t *deleted);

#endif
