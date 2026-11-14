#include "test.h"
#include <string.h>

static TestResult fragmented_pipeline(void) {
    TestResult result = TEST_PASS;
    const char *wire = "*1\r\n$4\r\nPING\r\n*1\r\n$4\r\nPING\r\n";
    const size_t frame_len = 14;
    RespCommand command = {0};
    size_t used = 999;
    for (size_t i = 0; i < frame_len; ++i) {
        EXPECT_STATUS(resp_parse((MrSlice){(const unsigned char *)wire, i}, &command, &used), MR_NEED_MORE);
        CHECK(used == 0);
    }
    EXPECT_STATUS(resp_parse(mr_slice(wire), &command, &used), MR_OK);
    CHECK(used == frame_len && command.argc == 1);
    CHECK(mr_slice_equal(command.args[0], mr_slice("PING")));
cleanup:
    return result;
}

static TestResult invalid_lengths(void) {
    TestResult result = TEST_PASS;
    RespCommand command = {0};
    size_t used = 0;
    EXPECT_STATUS(resp_parse(mr_slice("*1\r\n$-2\r\n"), &command, &used), MR_INVALID);
    EXPECT_STATUS(resp_parse(mr_slice("*1\r\n$x\r\n"), &command, &used), MR_INVALID);
    EXPECT_STATUS(resp_parse(mr_slice("*1\r\n$1048577\r\n"), &command, &used), MR_LIMIT);
cleanup:
    return result;
}

static TestResult binary_bulk_reply(void) {
    TestResult result = TEST_PASS;
    MrBuffer out = {0};
    const unsigned char payload[] = {'a', 0, '\r', '\n'};
    const unsigned char expected[] = {'$', '4', '\r', '\n', 'a', 0, '\r', '\n', '\r', '\n'};
    EXPECT_STATUS(resp_bulk((MrSlice){payload, sizeof(payload)}, &out), MR_OK);
    CHECK(out.len == sizeof(expected) && memcmp(out.data, expected, sizeof(expected)) == 0);
    mr_buffer_free(&out);
    EXPECT_STATUS(resp_null(&out), MR_OK);
    CHECK(mr_slice_equal((MrSlice){out.data, out.len}, mr_slice("$-1\r\n")));
cleanup:
    mr_buffer_free(&out);
    return result;
}

static TestResult binary_input_and_boundaries(void) {
    TestResult result = TEST_PASS;
    RespCommand command = {0};
    size_t used = 0;
    MrBuffer out = {0};
    const unsigned char wire[] = "*3\r\n$3\r\nSET\r\n$0\r\n\r\n$4\r\na\0\r\n\r\n";
    EXPECT_STATUS(resp_parse((MrSlice){wire, sizeof(wire) - 1}, &command, &used), MR_OK);
    CHECK(used == sizeof(wire) - 1 && command.args[1].len == 0 && command.args[2].len == 4);
    CHECK(command.args[2].data[1] == 0);
    EXPECT_STATUS(resp_parse(mr_slice("*9\r\n"), &command, &used), MR_LIMIT);
    EXPECT_STATUS(resp_parse(mr_slice("*1\r\n$1\r\nxXX"), &command, &used), MR_INVALID);
    EXPECT_STATUS(resp_parse(mr_slice("*9999999999999999999999999999"), &command, &used), MR_LIMIT);
    EXPECT_STATUS(resp_integer(INT64_MIN, &out), MR_OK);
    CHECK(mr_slice_equal((MrSlice){out.data, out.len}, mr_slice(":-9223372036854775808\r\n")));
    mr_buffer_free(&out);
    EXPECT_STATUS(resp_simple(mr_slice("bad\r\n"), &out), MR_INVALID);
    mr_alloc_fail_after(0);
    EXPECT_STATUS(resp_bulk(mr_slice("v"), &out), MR_OOM);
    CHECK(out.data == NULL && out.len == 0);
cleanup:
    mr_alloc_fail_after(-1);
    mr_buffer_free(&out);
    return result;
}

static TestResult random_parser_inputs(void) {
    TestResult result = TEST_PASS;
    uint32_t seed = 42;
    for (size_t length = 0; length < 128; ++length) {
        unsigned char bytes[128];
        for (int trial = 0; trial < 32; ++trial) {
            for (size_t i = 0; i < length; ++i) {
                seed = seed * 1664525u + 1013904223u;
                bytes[i] = (unsigned char)(seed >> 24);
            }
            RespCommand command;
            size_t used = 0;
            MrStatus status = resp_parse((MrSlice){bytes, length}, &command, &used);
            CHECK(status == MR_OK || status == MR_NEED_MORE || status == MR_INVALID || status == MR_LIMIT);
            CHECK(used <= length && (status == MR_OK || used == 0));
        }
    }
cleanup:
    return result;
}

void test_resp(void) {
    run_test("resp/every-prefix-and-pipeline", fragmented_pipeline);
    run_test("resp/malformed-and-oversized-length", invalid_lengths);
    run_test("resp/binary-bulk-and-null", binary_bulk_reply);
    run_test("resp/binary-request-and-boundaries", binary_input_and_boundaries);
    run_test("resp/4096-random-inputs", random_parser_inputs);
}
