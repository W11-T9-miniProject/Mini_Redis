#include "test.h"
#include <string.h>

static TestResult set_get_overwrite(void) {
    TestResult result = TEST_PASS;
    HashTable table = {0};
    EXPECT_STATUS(ht_init(&table, 8), MR_OK);
    EXPECT_STATUS(ht_set(&table, mr_slice("a"), mr_slice("one"), -1), MR_OK);
    HtEntry *entry = ht_find(&table, mr_slice("a"));
    CHECK(entry && mr_slice_equal((MrSlice){entry->value, entry->value_len}, mr_slice("one")));
    EXPECT_STATUS(ht_set(&table, mr_slice("a"), mr_slice("two"), 500), MR_OK);
    entry = ht_find(&table, mr_slice("a"));
    CHECK(table.size == 1);
    CHECK(entry && entry->expires_at_ms == 500);
    CHECK(mr_slice_equal((MrSlice){entry->value, entry->value_len}, mr_slice("two")));
cleanup:
    ht_destroy(&table);
    ht_destroy(&table); /* 중복 해제가 없어야 한다. */
    return result;
}

static TestResult collision_delete(void) {
    TestResult result = TEST_PASS;
    HashTable table = {0};
    /* 작은 버킷에서 시작해 자동 확장 후에도 명시적으로 충돌을 강제한다. */
    EXPECT_STATUS(ht_init(&table, 1), MR_OK);
    EXPECT_STATUS(ht_set(&table, mr_slice("a"), mr_slice("A"), -1), MR_OK);
    EXPECT_STATUS(ht_set(&table, mr_slice("b"), mr_slice("B"), -1), MR_OK);
    EXPECT_STATUS(ht_set(&table, mr_slice("c"), mr_slice("C"), -1), MR_OK);
    /* 확장 구현 후에도 충돌을 확실히 강제하려고 다시 버킷 하나로 모은다. */
    EXPECT_STATUS(ht_resize(&table, 1), MR_OK);
    EXPECT_STATUS(ht_delete(&table, mr_slice("b")), MR_OK);
    CHECK(!ht_find(&table, mr_slice("b")));
    CHECK(ht_find(&table, mr_slice("a")) && ht_find(&table, mr_slice("c")));
    EXPECT_STATUS(ht_delete(&table, mr_slice("a")), MR_OK);
    EXPECT_STATUS(ht_delete(&table, mr_slice("c")), MR_OK);
    EXPECT_STATUS(ht_delete(&table, mr_slice("c")), MR_NOT_FOUND);
    CHECK(table.size == 0);
cleanup:
    ht_destroy(&table);
    return result;
}

static TestResult binary_copy_and_resize(void) {
    TestResult result = TEST_PASS;
    HashTable table = {0};
    unsigned char key[] = {'k', 0, 'x'};
    unsigned char value[] = {'v', 0, 'x'};
    unsigned char original[] = {'k', 0, 'x'};
    EXPECT_STATUS(ht_init(&table, 2), MR_OK);
    EXPECT_STATUS(ht_set(&table, (MrSlice){key, sizeof(key)},
                         (MrSlice){value, sizeof(value)}, -1), MR_OK);
    key[0] = 'z'; value[0] = 'z'; /* 빌린 메모리를 저장했다면 여기서 깨진다. */
    EXPECT_STATUS(ht_set(&table, mr_slice("empty"), (MrSlice){NULL, 0}, -1), MR_OK);
    EXPECT_STATUS(ht_resize(&table, 32), MR_OK);
    HtEntry *entry = ht_find(&table, (MrSlice){original, sizeof(original)});
    CHECK(entry && entry->value_len == 3 && entry->value[0] == 'v' && entry->value[1] == 0);
    entry = ht_find(&table, mr_slice("empty"));
    CHECK(entry && entry->value_len == 0 && table.size == 2);
cleanup:
    ht_destroy(&table);
    return result;
}

static TestResult allocation_failure_rollback(void) {
    TestResult result = TEST_PASS;
    HashTable table = {0};
    mr_alloc_fail_after(0);
    EXPECT_STATUS(ht_init(&table, 1), MR_OOM);
    ht_destroy(&table);
    mr_alloc_fail_after(-1);
    EXPECT_STATUS(ht_init(&table, 1), MR_OK);
    EXPECT_STATUS(ht_set(&table, mr_slice("a"), mr_slice("old"), 123), MR_OK);
    mr_alloc_fail_after(0);
    EXPECT_STATUS(ht_set(&table, mr_slice("a"), mr_slice("new"), 999), MR_OOM);
    mr_alloc_fail_after(-1);
    /* 새 키의 값/노드/키/확장 배열, 각 할당 실패 경로를 전부 밟는다. */
    for (int64_t failure = 0; failure < 4; ++failure) {
        mr_alloc_fail_after(failure);
        EXPECT_STATUS(ht_set(&table, mr_slice("b"), mr_slice("v"), -1), MR_OOM);
        mr_alloc_fail_after(-1);
        HtEntry *entry = ht_find(&table, mr_slice("a"));
        CHECK(entry && entry->expires_at_ms == 123 && table.size == 1 && table.capacity == 1);
        CHECK(mr_slice_equal((MrSlice){entry->value, entry->value_len}, mr_slice("old")));
        CHECK(ht_find(&table, mr_slice("b")) == NULL);
    }
cleanup:
    mr_alloc_fail_after(-1);
    ht_destroy(&table);
    return result;
}

static TestResult many_keys_and_empty_key(void) {
    TestResult result = TEST_PASS;
    HashTable table = {0};
    EXPECT_STATUS(ht_init(&table, 0), MR_INVALID);
    EXPECT_STATUS(ht_init(&table, 2), MR_OK);
    EXPECT_STATUS(ht_set(&table, (MrSlice){NULL, 0}, mr_slice("empty-key"), -1), MR_OK);
    for (int i = 0; i < 2000; ++i) {
        char key[32];
        snprintf(key, sizeof(key), "key:%d", i);
        EXPECT_STATUS(ht_set(&table, mr_slice(key), mr_slice(key), -1), MR_OK);
    }
    CHECK(table.size == 2001 && table.capacity > 2);
    for (int i = 0; i < 2000; ++i) {
        char key[32];
        snprintf(key, sizeof(key), "key:%d", i);
        HtEntry *entry = ht_find(&table, mr_slice(key));
        CHECK(entry && mr_slice_equal((MrSlice){entry->value, entry->value_len}, mr_slice(key)));
        EXPECT_STATUS(ht_delete(&table, mr_slice(key)), MR_OK);
    }
    CHECK(table.size == 1 && table.scan_cursor->scan_next == table.scan_cursor);
    EXPECT_STATUS(ht_delete(&table, (MrSlice){NULL, 0}), MR_OK);
    CHECK(table.size == 0 && table.scan_cursor == NULL);
cleanup:
    ht_destroy(&table);
    return result;
}

void test_hash_table(void) {
    run_test("hash/set-get-overwrite", set_get_overwrite);
    run_test("hash/collision-delete", collision_delete);
    run_test("hash/binary-copy-resize", binary_copy_and_resize);
    run_test("hash/allocation-failure-rollback", allocation_failure_rollback);
    run_test("hash/2000-keys-and-empty-key", many_keys_and_empty_key);
}
