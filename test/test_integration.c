/*
 * test_integration.c — Full-stack integration tests
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include "novadb/nvdb.h"
#include "novadb/types.h"

#define TMP_DIR_TEMPLATE "/tmp/novadb-test-int-XXXXXX"

static int test_full_sql_cycle(void) {
    char tmpdir[] = TMP_DIR_TEMPLATE;
    if (!mkdtemp(tmpdir)) { perror("mkdtemp"); return 1; }

    NVDBEngine *eng = nvdb_open(tmpdir);
    assert(eng != NULL);

    /* CREATE TABLE */
    NVDBResultSet *rs = nvdb_exec(eng,
        "CREATE TABLE employees ("
        "  id INTEGER PRIMARY KEY,"
        "  name VARCHAR(255) NOT NULL,"
        "  salary FLOAT"
        ")");
    assert(rs != NULL);
    assert(!rs->error);
    printf("    CREATE TABLE: %s\n", rs->message);
    nvdb_result_free(rs);

    /* INSERT */
    rs = nvdb_exec(eng,
        "INSERT INTO employees (id, name, salary) VALUES (1, 'Alice', 75000)");
    assert(rs != NULL && !rs->error);
    printf("    INSERT: %s\n", rs->message);
    nvdb_result_free(rs);

    rs = nvdb_exec(eng,
        "INSERT INTO employees (id, name, salary) VALUES (2, 'Bob', 82000)");
    assert(rs != NULL && !rs->error);
    nvdb_result_free(rs);

    rs = nvdb_exec(eng,
        "INSERT INTO employees (id, name, salary) VALUES (3, 'Carol', 95000)");
    assert(rs != NULL && !rs->error);
    nvdb_result_free(rs);

    /* SELECT * */
    rs = nvdb_exec(eng, "SELECT * FROM employees");
    assert(rs != NULL && !rs->error);
    printf("    SELECT *: %d rows, %d cols\n", rs->nrows, rs->ncols);
    assert(rs->nrows == 3);
    for (int r = 0; r < rs->nrows; r++) {
        printf("      row %d: ", r);
        for (int c = 0; c < rs->ncols; c++) {
            const NVDBValue *v = nvdb_result_value(rs, (uint64_t)r, (uint32_t)c);
            if (v && v->type == NVDB_TYPE_INT64)
                printf("%" PRId64 " ", v->i64);
            else if (v && v->type == NVDB_TYPE_STRING)
                printf("%s ", v->str_val ? v->str_val : "NULL");
            else
                printf("? ");
        }
        printf("\n");
    }
    nvdb_result_free(rs);

    /* SELECT with WHERE */
    rs = nvdb_exec(eng, "SELECT name, salary FROM employees WHERE salary > 80000");
    assert(rs != NULL && !rs->error);
    printf("    SELECT WHERE salary > 80000: %d rows\n", rs->nrows);
    assert(rs->nrows == 2);
    nvdb_result_free(rs);

    /* UPDATE */
    rs = nvdb_exec(eng, "UPDATE employees SET salary = 78000 WHERE name = 'Alice'");
    assert(rs != NULL && !rs->error);
    printf("    UPDATE: %s\n", rs->message);
    nvdb_result_free(rs);

    /* Verify UPDATE */
    rs = nvdb_exec(eng, "SELECT salary FROM employees WHERE name = 'Alice'");
    assert(rs != NULL && !rs->error);
    if (rs->nrows > 0) {
        const NVDBValue *v = nvdb_result_value(rs, 0, 0);
        assert(v && v->type == NVDB_TYPE_INT64 && v->i64 == 78000);
        printf("    Verified Alice salary: %" PRId64 "\n", v->i64);
    }
    nvdb_result_free(rs);

    /* DELETE */
    rs = nvdb_exec(eng, "DELETE FROM employees WHERE name = 'Bob'");
    assert(rs != NULL && !rs->error);
    printf("    DELETE: %s\n", rs->message);
    nvdb_result_free(rs);

    /* Verify DELETE */
    rs = nvdb_exec(eng, "SELECT * FROM employees");
    assert(rs != NULL && !rs->error);
    printf("    After DELETE: %d rows\n", rs->nrows);
    assert(rs->nrows == 2);
    nvdb_result_free(rs);

    /* DROP TABLE */
    rs = nvdb_exec(eng, "DROP TABLE employees");
    assert(rs != NULL && !rs->error);
    printf("    DROP TABLE: %s\n", rs->message);
    nvdb_result_free(rs);

    /* SELECT after DROP should fail */
    rs = nvdb_exec(eng, "SELECT * FROM employees");
    assert(rs != NULL && rs->error);
    printf("    SELECT after DROP correctly errors: %s\n", rs->error_msg);
    nvdb_result_free(rs);

    nvdb_close(eng);

    /* Cleanup */
    {
        char dbpath[256];
        snprintf(dbpath, sizeof(dbpath), "%s/data.db", tmpdir);
        unlink(dbpath);
    }
    rmdir(tmpdir);

    return 0;
}

int test_integration(void) {
    if (test_full_sql_cycle() != 0) {
        printf("    full_sql_cycle FAILED\n");
        return 1;
    }
    return 0;
}
