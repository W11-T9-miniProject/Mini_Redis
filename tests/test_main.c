#include "test.h"
#include <string.h>

static unsigned passed, failed;

void run_test(const char *name, TestFunction function) {
    TestResult result = function();
    const char *label = "FAIL";
    if (result == TEST_PASS) { ++passed; label = "PASS"; }
    else ++failed;
    printf("[%s] %s\n", label, name);
}

int test_summary(void) {
    printf("\nPASS=%u FAIL=%u\n", passed, failed);
    return failed ? 1 : 0;
}

int main(int argc, char **argv) {
    const char *stage = argc == 1 ? "all" : argv[1];
    bool all = strcmp(stage, "all") == 0;
    if (argc > 2 || (!all && strcmp(stage, "hash") && strcmp(stage, "store") &&
                    strcmp(stage, "resp") && strcmp(stage, "command"))) {
        fprintf(stderr, "Usage: unit-tests [all|hash|store|resp|command]\n");
        return 2;
    }
    if (all || !strcmp(stage, "hash")) test_hash_table();
    if (all || !strcmp(stage, "store")) test_store();
    if (all || !strcmp(stage, "resp")) test_resp();
    if (all || !strcmp(stage, "command")) test_command();
    return test_summary();
}
