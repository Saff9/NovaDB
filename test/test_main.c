/*
 * test_main.c — Test runner for NovaDB
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern int test_btree(void);
extern int test_wal(void);
extern int test_sql(void);
extern int test_integration(void);

typedef struct {
    const char *name;
    int (*func)(void);
} TestCase;

static TestCase g_tests[] = {
    {"BTree storage engine",  test_btree},
    {"Write-Ahead Log",       test_wal},
    {"SQL parser & executor", test_sql},
    {"Full integration",      test_integration},
};

int main(void) {
    int passed = 0, failed = 0;

    printf("NovaDB Test Suite\n");
    printf("=================\n\n");

    for (size_t i = 0; i < sizeof(g_tests) / sizeof(g_tests[0]); i++) {
        printf("  [ RUN      ] %s\n", g_tests[i].name);
        int rc = g_tests[i].func();
        if (rc == 0) {
            printf("  [       OK ] %s\n", g_tests[i].name);
            passed++;
        } else {
            printf("  [     FAIL ] %s\n", g_tests[i].name);
            failed++;
        }
    }

    printf("\n%d passed, %d failed\n", passed, failed);
    return failed > 0 ? 1 : 0;
}
