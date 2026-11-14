#ifndef MINI_REDIS_TEST_H
#define MINI_REDIS_TEST_H

#include "mini_redis/command.h"
#include <stdio.h>

/* 각 테스트는 fixture를 정리한 뒤 반환한다. 실패해도 메모리 검증을 이어간다. */
typedef enum { TEST_PASS, TEST_FAIL } TestResult;
typedef TestResult (*TestFunction)(void);

void run_test(const char *name, TestFunction function);
int test_summary(void);
void test_hash_table(void);
void test_store(void);
void test_resp(void);
void test_command(void);

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "  %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        result = TEST_FAIL; \
        goto cleanup; \
    } \
} while (0)

#define EXPECT_STATUS(expression, expected) do { \
    MrStatus actual_status = (expression); \
    if (actual_status != (expected)) { \
        fprintf(stderr, "  %s:%d: %s: expected %s, got %s\n", \
            __FILE__, __LINE__, #expression, mr_status_name(expected), \
            mr_status_name(actual_status)); \
        result = TEST_FAIL; \
        goto cleanup; \
    } \
} while (0)

#endif
