/*
 * test_btree.c — B+Tree correctness tests
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include "novadb/types.h"
#include "storage/page.h"
#include "storage/buffer.h"
#include "storage/btree.h"

#define TMP_DIR_TEMPLATE "/tmp/novadb-test-btree-XXXXXX"

static int test_basic_crud(void) {
    char tmpdir[] = TMP_DIR_TEMPLATE;
    if (!mkdtemp(tmpdir)) { perror("mkdtemp"); return 1; }

    char dbpath[256];
    snprintf(dbpath, sizeof(dbpath), "%s/data.db", tmpdir);

    NVDBBufferPool *bp = bp_create(dbpath);
    assert(bp != NULL);

    NVDBBTree *tree = btree_open(bp);
    assert(tree != NULL);

    /* Insert */
    int rc = btree_insert(tree, "hello", 5, "world", 5);
    assert(rc == NVDB_OK);

    /* Search */
    char value[256];
    uint16_t vlen;
    rc = btree_search(tree, "hello", 5, value, &vlen, sizeof(value));
    assert(rc == NVDB_OK);
    assert(vlen == 5);
    assert(memcmp(value, "world", 5) == 0);

    /* Update */
    rc = btree_insert(tree, "hello", 5, "nova!", 5);
    assert(rc == NVDB_OK);
    rc = btree_search(tree, "hello", 5, value, &vlen, sizeof(value));
    assert(rc == NVDB_OK);
    assert(memcmp(value, "nova!", 5) == 0);

    /* Delete */
    rc = btree_delete(tree, "hello", 5);
    assert(rc == NVDB_OK);
    rc = btree_search(tree, "hello", 5, value, &vlen, sizeof(value));
    assert(rc == NVDB_ERR_NOT_FOUND);

    /* Not found */
    rc = btree_search(tree, "nonexistent", 11, value, &vlen, sizeof(value));
    assert(rc == NVDB_ERR_NOT_FOUND);

    btree_close(tree);
    bp_destroy(bp);

    /* Cleanup */
    unlink(dbpath);
    rmdir(tmpdir);

    return 0;
}

static int test_many_keys(void) {
    char tmpdir[] = TMP_DIR_TEMPLATE;
    if (!mkdtemp(tmpdir)) { perror("mkdtemp"); return 1; }

    char dbpath[256];
    snprintf(dbpath, sizeof(dbpath), "%s/data.db", tmpdir);

    NVDBBufferPool *bp = bp_create(dbpath);
    assert(bp != NULL);

    NVDBBTree *tree = btree_open(bp);
    assert(tree != NULL);

    /* Insert 500 keys to exercise splitting */
    int count = 500;
    for (int i = 0; i < count; i++) {
        char key[32], val[32];
        snprintf(key, sizeof(key), "key-%05d", i);
        snprintf(val, sizeof(val), "value-%05d", i);
        int rc = btree_insert(tree, key, (uint16_t)strlen(key),
                               val, (uint16_t)strlen(val));
        assert(rc == NVDB_OK);
    }

    /* Read them all back */
    for (int i = 0; i < count; i++) {
        char key[32], expected[32];
        snprintf(key, sizeof(key), "key-%05d", i);
        snprintf(expected, sizeof(expected), "value-%05d", i);

        char value[256];
        uint16_t vlen;
        int rc = btree_search(tree, key, (uint16_t)strlen(key),
                               value, &vlen, sizeof(value));
        if (rc != NVDB_OK) {
            fprintf(stderr, "key '%s' not found after insert\n", key);
            return 1;
        }
        value[vlen] = '\0';
        if (strcmp(value, expected) != 0) {
            fprintf(stderr, "key '%s': expected '%s', got '%s'\n",
                    key, expected, value);
            return 1;
        }
    }

    printf("    Inserted and verified %d keys\n", count);

    btree_close(tree);
    bp_destroy(bp);

    unlink(dbpath);
    rmdir(tmpdir);

    return 0;
}

int test_btree(void) {
    int failures = 0;

    if (test_basic_crud() != 0) {
        printf("    basic_crud FAILED\n");
        failures++;
    }
    if (test_many_keys() != 0) {
        printf("    many_keys FAILED\n");
        failures++;
    }

    return failures;
}
