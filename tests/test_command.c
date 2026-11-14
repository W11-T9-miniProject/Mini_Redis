#include "test.h"

static TestResult execute_flow(void) {
    TestResult result = TEST_PASS;
    Store store = {0};
    MrBuffer reply = {0};
    RespCommand command = {0};
    EXPECT_STATUS(store_init(&store, 8), MR_OK);
    command.argc = 3;
    command.args[0] = mr_slice("set");
    command.args[1] = mr_slice("key");
    command.args[2] = mr_slice("value");
    EXPECT_STATUS(command_execute(&store, &command, 100, &reply), MR_OK);
    CHECK(mr_slice_equal((MrSlice){reply.data, reply.len}, mr_slice("+OK\r\n")));
    mr_buffer_free(&reply);
    command.argc = 2;
    command.args[0] = mr_slice("GET");
    EXPECT_STATUS(command_execute(&store, &command, 101, &reply), MR_OK);
    CHECK(mr_slice_equal((MrSlice){reply.data, reply.len}, mr_slice("$5\r\nvalue\r\n")));
    mr_buffer_free(&reply);
    command.args[0] = mr_slice("DEL");
    EXPECT_STATUS(command_execute(&store, &command, 102, &reply), MR_OK);
    CHECK(mr_slice_equal((MrSlice){reply.data, reply.len}, mr_slice(":1\r\n")));
    mr_buffer_free(&reply);
    command.args[0] = mr_slice("GET");
    EXPECT_STATUS(command_execute(&store, &command, 103, &reply), MR_OK);
    CHECK(mr_slice_equal((MrSlice){reply.data, reply.len}, mr_slice("$-1\r\n")));
cleanup:
    mr_buffer_free(&reply);
    store_destroy(&store);
    return result;
}

static TestResult error_without_mutation(void) {
    TestResult result = TEST_PASS;
    Store store = {0};
    MrBuffer reply = {0};
    RespCommand command = {0};
    MrSlice value = {0};
    EXPECT_STATUS(store_init(&store, 8), MR_OK);
    EXPECT_STATUS(store_set(&store, mr_slice("key"), mr_slice("old"), -1, 0), MR_OK);
    command.argc = 5;
    command.args[0] = mr_slice("SET");
    command.args[1] = mr_slice("key");
    command.args[2] = mr_slice("bad");
    command.args[3] = mr_slice("PX");
    command.args[4] = mr_slice("0");
    EXPECT_STATUS(command_execute(&store, &command, 1, &reply), MR_OK);
    CHECK(reply.len >= 5 && reply.data[0] == '-');
    EXPECT_STATUS(store_get(&store, mr_slice("key"), 1, &value), MR_OK);
    CHECK(mr_slice_equal(value, mr_slice("old")));
cleanup:
    mr_buffer_free(&reply);
    store_destroy(&store);
    return result;
}

static TestResult command_options_and_errors(void) {
    TestResult result = TEST_PASS;
    Store store = {0};
    MrBuffer reply = {0};
    RespCommand command = {0};
    EXPECT_STATUS(store_init(&store, 8), MR_OK);
    command.argc = 1; command.args[0] = mr_slice("pInG");
    EXPECT_STATUS(command_execute(&store, &command, 0, &reply), MR_OK);
    CHECK(mr_slice_equal((MrSlice){reply.data, reply.len}, mr_slice("+PONG\r\n")));
    mr_buffer_free(&reply);
    command.args[0] = mr_slice("UNKNOWN");
    EXPECT_STATUS(command_execute(&store, &command, 0, &reply), MR_OK);
    CHECK(reply.data[0] == '-');
    mr_buffer_free(&reply);
    command.argc = 5;
    command.args[0] = mr_slice("SET"); command.args[1] = mr_slice("k"); command.args[2] = mr_slice("v");
    command.args[3] = mr_slice("px"); command.args[4] = mr_slice("1500");
    EXPECT_STATUS(command_execute(&store, &command, 100, &reply), MR_OK);
    mr_buffer_free(&reply);
    command.argc = 2; command.args[0] = mr_slice("TTL");
    EXPECT_STATUS(command_execute(&store, &command, 101, &reply), MR_OK);
    CHECK(mr_slice_equal((MrSlice){reply.data, reply.len}, mr_slice(":1\r\n")));
    mr_buffer_free(&reply);
    command.argc = 3; command.args[0] = mr_slice("EXPIRE"); command.args[2] = mr_slice("-9223372036854775808");
    EXPECT_STATUS(command_execute(&store, &command, 101, &reply), MR_OK);
    CHECK(mr_slice_equal((MrSlice){reply.data, reply.len}, mr_slice(":1\r\n")));
    mr_buffer_free(&reply);
    command.args[2] = mr_slice("9223372036854775808");
    EXPECT_STATUS(command_execute(&store, &command, 101, &reply), MR_OK);
    CHECK(reply.data[0] == '-');
    mr_buffer_free(&reply);
    command.argc = 2; command.args[0] = mr_slice("TTL");
    EXPECT_STATUS(command_execute(&store, &command, 101, &reply), MR_OK);
    CHECK(mr_slice_equal((MrSlice){reply.data, reply.len}, mr_slice(":-2\r\n")));
cleanup:
    mr_buffer_free(&reply);
    store_destroy(&store);
    return result;
}

void test_command(void) {
    run_test("command/set-get-del", execute_flow);
    run_test("command/invalid-px-does-not-mutate", error_without_mutation);
    run_test("command/options-integer-boundaries-and-errors", command_options_and_errors);
}
