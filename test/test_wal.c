/*
 * test_wal.c — WAL correctness and recovery tests
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
#include "storage/wal.h"

#define TMP_DIR_TEMPLATE "/tmp/novadb-test-wal-XXXXXX"

static int test_wal_append_recover(void) {
    char tmpdir[] = TMP_DIR_TEMPLATE;
    if (!mkdtemp(tmpdir)) { perror("mkdtemp"); return 1; }

    char dbpath[256];
    snprintf(dbpath, sizeof(dbpath), "%s/data.db", tmpdir);

    /* Phase 1: write data with WAL */
    {
        NVDBBufferPool *bp = bp_create(dbpath);
        assert(bp != NULL);

        NVDBBTree *tree = btree_open(bp);
        assert(tree != NULL);

        NVDBWAL *wal = wal_open(tmpdir);
        assert(wal != NULL);

        /* Insert with WAL logging */
        btree_insert(tree, "key1", 4, "val1", 4);
        wal_append(wal, NVDB_WAL_INSERT, 1, "key1", 4, "val1", 4);
        wal_append(wal, NVDB_WAL_COMMIT, 1, NULL, 0, NULL, 0);
        wal_sync(wal);

        btree_insert(tree, "key2", 4, "val2", 4);
        wal_append(wal, NVDB_WAL_INSERT, 2, "key2", 4, "val2", 4);
        wal_append(wal, NVDB_WAL_COMMIT, 2, NULL, 0, NULL, 0);
        wal_sync(wal);

        wal_close(wal);
        btree_close(tree);
        bp_destroy(bp);
    }

    /* Phase 2: recover from WAL */
    {
        NVDBBufferPool *bp2 = bp_create(dbpath);
        assert(bp2 != NULL);

        NVDBBTree *tree2 = btree_open(bp2);
        assert(tree2 != NULL);

        NVDBWAL *wal2 = wal_open(tmpdir);
        assert(wal2 != NULL);

        int rc = wal_recover(wal2, tree2);
        assert(rc == NVDB_OK);

        /* Verify recovered data */
        char value[256];
        uint16_t vlen;

        rc = btree_search(tree2, "key1", 4, value, &vlen, sizeof(value));
        assert(rc == NVDB_OK);
        assert(vlen == 4 && memcmp(value, "val1", 4) == 0);

        rc = btree_search(tree2, "key2", 4, value, &vlen, sizeof(value));
        assert(rc == NVDB_OK);
        assert(vlen == 4 && memcmp(value, "val2", 4) == 0);

        wal_close(wal2);
        btree_close(tree2);
        bp_destroy(bp2);
    }

    unlink(dbpath);
    rmdir(tmpdir);

    return 0;
}

int test_wal(void) {
    if (test_wal_append_recover() != 0) {
        printf("    wal_append_recover FAILED\n");
        return 1;
    }
    return 0;
}
