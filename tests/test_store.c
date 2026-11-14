#include "test.h"
#include <stdint.h>

static TestResult expiry_boundary(void) {
    TestResult result = TEST_PASS;
    Store store = {0};
    MrSlice value = {0};
    EXPECT_STATUS(store_init(&store, 8), MR_OK);
    EXPECT_STATUS(store_set(&store, mr_slice("k"), mr_slice("v"), 500, 1000), MR_OK);
    EXPECT_STATUS(store_get(&store, mr_slice("k"), 1499, &value), MR_OK);
    CHECK(mr_slice_equal(value, mr_slice("v")));
    EXPECT_STATUS(store_get(&store, mr_slice("k"), 1500, &value), MR_NOT_FOUND);
    CHECK(value.data == NULL && store.table.size == 0);
cleanup:
    store_destroy(&store);
    return result;
}

static TestResult overwrite_and_expire(void) {
    TestResult result = TEST_PASS;
    Store store = {0};
    int64_t ttl = 0;
    EXPECT_STATUS(store_init(&store, 8), MR_OK);
    EXPECT_STATUS(store_set(&store, mr_slice("k"), mr_slice("old"), 10, 100), MR_OK);
    EXPECT_STATUS(store_set(&store, mr_slice("k"), mr_slice("new"), -1, 101), MR_OK);
    EXPECT_STATUS(store_ttl(&store, mr_slice("k"), 1000, &ttl), MR_OK);
    CHECK(ttl == -1);
    EXPECT_STATUS(store_expire(&store, mr_slice("k"), 0, 1000), MR_OK);
    EXPECT_STATUS(store_ttl(&store, mr_slice("k"), 1000, &ttl), MR_NOT_FOUND);
    CHECK(ttl == -2);
    EXPECT_STATUS(store_delete(&store, mr_slice("k"), 1000), MR_NOT_FOUND);
cleanup:
    store_destroy(&store);
    return result;
}

static TestResult overflow_preserves_value(void) {
    TestResult result = TEST_PASS;
    Store store = {0};
    MrSlice value = {0};
    EXPECT_STATUS(store_init(&store, 8), MR_OK);
    EXPECT_STATUS(store_set(&store, mr_slice("k"), mr_slice("old"), -1, 0), MR_OK);
    EXPECT_STATUS(store_set(&store, mr_slice("k"), mr_slice("bad"), 10, INT64_MAX - 5), MR_INVALID);
    EXPECT_STATUS(store_get(&store, mr_slice("k"), 1, &value), MR_OK);
    CHECK(mr_slice_equal(value, mr_slice("old")));
cleanup:
    store_destroy(&store);
    return result;
}

static TestResult active_expiry(void) {
    TestResult result = TEST_PASS;
    Store store = {0};
    size_t deleted = 0, total = 0;
    EXPECT_STATUS(store_init(&store, 8), MR_OK);
    EXPECT_STATUS(store_set(&store, mr_slice("dead"), mr_slice("v"), 5, 100), MR_OK);
    EXPECT_STATUS(store_set(&store, mr_slice("live"), mr_slice("v"), -1, 100), MR_OK);
    EXPECT_STATUS(store_sweep(&store, 105, 0, &deleted), MR_OK);
    CHECK(deleted == 0 && store.table.size == 2);
    for (size_t i = 0; i < 32; ++i) {
        EXPECT_STATUS(store_sweep(&store, 105, 4, &deleted), MR_OK);
        total += deleted;
    }
    CHECK(total == 1 && store.table.size == 1);
    CHECK(ht_find(&store.table, mr_slice("live")) != NULL);
cleanup:
    store_destroy(&store);
    return result;
}

static TestResult sweep_budget_resize_and_delete(void) {
    TestResult result = TEST_PASS;
    Store store = {0};
    size_t deleted = 0;
    int64_t ttl = 0;
    EXPECT_STATUS(store_init(&store, 2), MR_OK);
    for (int i = 0; i < 100; ++i) {
        char key[32];
        snprintf(key, sizeof(key), "key:%d", i);
        EXPECT_STATUS(store_set(&store, mr_slice(key), mr_slice("v"), 10, 0), MR_OK);
    }
    EXPECT_STATUS(store_set(&store, mr_slice("live"), mr_slice("v"), -1, 0), MR_OK);
    EXPECT_STATUS(ht_resize(&store.table, 1), MR_OK); /* 모든 노드가 한 충돌 체인 */
    EXPECT_STATUS(store_sweep(&store, 10, 1, &deleted), MR_OK);
    CHECK(store.last_sweep_checked == 1);
    EXPECT_STATUS(ht_delete(&store.table, mr_slice("key:50")), MR_OK);
    EXPECT_STATUS(ht_resize(&store.table, 64), MR_OK); /* 도중 재해시 */
    for (size_t i = 0; i < 150; ++i) {
        EXPECT_STATUS(store_sweep(&store, 10, 1, &deleted), MR_OK);
        CHECK(store.last_sweep_checked <= 1 && deleted <= 1);
    }
    CHECK(store.table.size == 1 && ht_find(&store.table, mr_slice("live")));
    EXPECT_STATUS(store_expire(&store, mr_slice("live"), 50, 100), MR_OK);
    EXPECT_STATUS(store_ttl(&store, mr_slice("live"), 110, &ttl), MR_OK);
    CHECK(ttl == 40);
    EXPECT_STATUS(store_delete(&store, mr_slice("live"), 150), MR_NOT_FOUND);
    EXPECT_STATUS(store_set(&store, mr_slice("k"), mr_slice("v"), 0, 0), MR_INVALID);
    EXPECT_STATUS(store_set(&store, mr_slice("k"), mr_slice("v"), -1, -1), MR_INVALID);
    EXPECT_STATUS(store_set(&store, mr_slice("k"), mr_slice("v"), -1, 0), MR_OK);
    EXPECT_STATUS(store_expire(&store, mr_slice("k"), INT64_MIN, 0), MR_OK);
    CHECK(store.table.size == 0);
cleanup:
    store_destroy(&store);
    return result;
}

void test_store(void) {
    run_test("store/expiry-exact-boundary", expiry_boundary);
    run_test("store/overwrite-clears-ttl-and-expire-zero", overwrite_and_expire);
    run_test("store/overflow-preserves-old-value", overflow_preserves_value);
    run_test("store/sweep-without-get", active_expiry);
    run_test("store/sweep-budget-resize-delete-and-inputs", sweep_budget_resize_and_delete);
}
