#ifndef MINI_REDIS_COMMAND_H
#define MINI_REDIS_COMMAND_H

#include "mini_redis/resp.h"
#include "mini_redis/store.h"

/* now_ms는 서버가 명령 실행 직전에 읽어 전달. 테스트는 숫자를 직접 전달.
 * 프로토콜 수준 오류 응답 생성 성공도 MR_OK다. 실제 실패와 구분한다.
 * 예: 알 수 없는 명령 → MR_OK + '-ERR ...\r\n'. OOM → MR_OOM.
 * reply는 호출자가 소유. 함수 호출 전 {0}, 사용 후 mr_buffer_free(). */
MrStatus command_execute(Store *store, const RespCommand *command,
                         int64_t now_ms, MrBuffer *reply);

#endif
