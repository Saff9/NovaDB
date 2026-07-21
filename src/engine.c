/*
 * engine.c — Database engine implementation
 *
 * Contains: NVDBEngine struct, public API, engine bridge functions.
 * This file is linked into both the server binary and test suite.
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <inttypes.h>
#include "novadb/nvdb.h"
#include "novadb/types.h"
#include "novadb/error.h"

#include "common/memory.h"
#include "common/logging.h"
#include "storage/page.h"
#include "storage/buffer.h"
#include "storage/btree.h"
#include "storage/wal.h"
#include "txn/transaction.h"
#include "sql/lexer.h"
#include "sql/parser.h"
#include "sql/executor.h"

/* ── Engine struct ────────────────────────────────────────────── */

struct NVDBEngine {
    char             data_dir[512];
    NVDBBufferPool  *bp;
    NVDBBTree       *btree;
    NVDBWAL         *wal;
    NVDBTxnMgr      *txnmgr;
    NVDBTransaction *active_txn;
    int64_t          row_counters[256];
    char            *table_names[256];
    int              ntable_counters;
};

/* ── Engine bridge (called by executor + transaction modules) ─── */

int engine_btree_insert(void *eng, const void *k, uint16_t kl,
                         const void *v, uint16_t vl) {
    NVDBEngine *e = (NVDBEngine *)eng;
    return btree_insert(e->btree, k, kl, v, vl);
}

int engine_btree_delete(void *eng, const void *k, uint16_t kl) {
    NVDBEngine *e = (NVDBEngine *)eng;
    return btree_delete(e->btree, k, kl);
}

int engine_btree_search(void *eng, const void *k, uint16_t kl,
                         void *v, uint16_t *vl, uint16_t max) {
    NVDBEngine *e = (NVDBEngine *)eng;
    return btree_search(e->btree, k, kl, v, vl, max);
}

int engine_wal_log(void *eng, nvdb_walop_t op, txnid_t tid,
                    const void *k, uint16_t kl,
                    const void *v, uint32_t vl) {
    NVDBEngine *e = (NVDBEngine *)eng;
    return wal_append(e->wal, op, tid, k, kl, v, vl);
}

int engine_wal_sync(void *eng) {
    NVDBEngine *e = (NVDBEngine *)eng;
    return wal_sync(e->wal);
}

int64_t engine_next_rowid(void *eng, const char *table) {
    NVDBEngine *e = (NVDBEngine *)eng;
    for (int i = 0; i < e->ntable_counters; i++) {
        if (strcmp(e->table_names[i], table) == 0)
            return ++e->row_counters[i];
    }
    if (e->ntable_counters < 256) {
        int idx = e->ntable_counters++;
        e->table_names[idx] = nvdb_strdup(table);
        e->row_counters[idx] = 1;
        return 1;
    }
    return 1;
}

/* ── Public API ───────────────────────────────────────────────── */

NVDBEngine *nvdb_open(const char *path) {
    NVDBEngine *eng = nvdb_calloc(1, sizeof(*eng));
    snprintf(eng->data_dir, sizeof(eng->data_dir), "%s", path);

    char db_path[512];
    snprintf(db_path, sizeof(db_path), "%s/data.db", path);
    eng->bp = bp_create(db_path);
    if (!eng->bp) { nvdb_log_error("cannot open buffer pool"); free(eng); return NULL; }

    eng->btree = btree_open(eng->bp);
    if (!eng->btree) { nvdb_log_error("cannot open btree"); bp_destroy(eng->bp); free(eng); return NULL; }

    eng->wal = wal_open(path);
    if (!eng->wal) { nvdb_log_error("cannot open WAL"); btree_close(eng->btree); bp_destroy(eng->bp); free(eng); return NULL; }

    int rc = wal_recover(eng->wal, eng->btree);
    if (rc != NVDB_OK)
        nvdb_log_warn("WAL recovery: %s", nvdb_strerror(rc));

    eng->txnmgr = txnmgr_create(eng);
    nvdb_log_notice("engine opened: %s", path);
    return eng;
}

nvdb_errcode_t nvdb_close(NVDBEngine *eng) {
    if (!eng) return NVDB_OK;
    bp_flush_all(eng->bp);
    wal_sync(eng->wal);
    wal_truncate(eng->wal);
    txnmgr_destroy(eng->txnmgr);
    wal_close(eng->wal);
    btree_close(eng->btree);
    bp_destroy(eng->bp);
    for (int i = 0; i < eng->ntable_counters; i++) free(eng->table_names[i]);
    free(eng);
    nvdb_log_notice("engine closed");
    return NVDB_OK;
}

NVDBTransaction *nvdb_begin(NVDBEngine *engine) {
    return txnmgr_begin(engine->txnmgr, NVDB_ISO_SNAPSHOT);
}

nvdb_errcode_t nvdb_commit(NVDBTransaction *txn) {
    return txnmgr_commit(txn);
}

nvdb_errcode_t nvdb_rollback(NVDBTransaction *txn) {
    txnmgr_rollback(txn);
    return NVDB_OK;
}

NVDBResultSet *nvdb_exec(NVDBEngine *engine, const char *sql) {
    SQLLexer *lex = lexer_create(sql);
    parser_reset();
    ASTStmt *stmt = parser_parse(lex);
    NVDBResultSet *rs = nvdb_calloc(1, sizeof(*rs));

    if (!stmt) {
        rs->error = true;
        rs->error_msg = nvdb_strdup(nvdb_strerror(nvdb_last_error.code));
        lexer_destroy(lex);
        return rs;
    }

    ExecResult *res = executor_run(engine, stmt);
    if (res->error) {
        rs->error = true;
        rs->error_msg = nvdb_strdup(res->error);
    } else {
        rs->ncols = res->ncols;
        rs->col_names = nvdb_calloc((size_t)res->ncols, sizeof(char *));
        for (int i = 0; i < res->ncols; i++)
            rs->col_names[i] = nvdb_strdup(res->col_names[i]);
        rs->nrows = res->nrows;
        rs->rows  = nvdb_calloc((size_t)res->nrows, sizeof(NVDBValue *));
        for (int r = 0; r < res->nrows; r++) {
            rs->rows[r] = nvdb_calloc((size_t)res->ncols, sizeof(NVDBValue));
            for (int c = 0; c < res->ncols; c++) {
                NVDBValue *src = &res->rows[r][c];
                rs->rows[r][c] = *src;
                /* Deep-copy strings: executor_free_result will free
                   the originals, so we must own our own copy. */
                if (src->type == NVDB_TYPE_STRING && src->str_val) {
                    rs->rows[r][c].str_val = nvdb_strdup(src->str_val);
                    rs->rows[r][c].str_len = src->str_len;
                }
            }
        }
        rs->message = nvdb_strdup(res->message ? res->message : "");
    }
    executor_free_result(res);
    lexer_destroy(lex);
    return rs;
}

NVDBResultSet *nvdb_exec_txn(NVDBTransaction *txn, const char *sql) {
    (void)txn; (void)sql;
    return NULL;
}

uint32_t nvdb_result_ncols(const NVDBResultSet *rs) {
    return rs ? (uint32_t)rs->ncols : 0;
}

uint64_t nvdb_result_nrows(const NVDBResultSet *rs) {
    return rs ? (uint64_t)rs->nrows : 0;
}

const char *nvdb_result_colname(const NVDBResultSet *rs, uint32_t col) {
    if (!rs || (int)col >= rs->ncols) return NULL;
    return rs->col_names[col];
}

const NVDBValue *nvdb_result_value(const NVDBResultSet *rs,
                                    uint64_t row, uint32_t col) {
    if (!rs || (int)row >= rs->nrows || (int)col >= rs->ncols) return NULL;
    return &rs->rows[row][col];
}

void nvdb_result_free(NVDBResultSet *rs) {
    if (!rs) return;
    free(rs->error_msg); free(rs->message);
    if (rs->col_names) {
        for (int i = 0; i < rs->ncols; i++) free(rs->col_names[i]);
        free(rs->col_names);
    }
    if (rs->rows) {
        for (int r = 0; r < rs->nrows; r++) free(rs->rows[r]);
        free(rs->rows);
    }
    free(rs);
}

const NVDBStats *nvdb_stats(NVDBEngine *engine) {
    static NVDBStats st; memset(&st, 0, sizeof(st));
    if (engine) bp_stats(engine->bp, &st.buffer_pool_hits, &st.buffer_pool_misses);
    return &st;
}

const char *nvdb_version(void) {
    return "NovaDB 1.0.0";
}

nvdb_errcode_t nvdb_kv_get(NVDBEngine *engine,
                            const char *key, char **value, size_t *vallen) {
    uint16_t vl; char buf[8192];
    int rc = btree_search(engine->btree, key, (uint16_t)strlen(key),
                           buf, &vl, sizeof(buf) - 1);
    if (rc != NVDB_OK) return (nvdb_errcode_t)rc;
    *value  = nvdb_malloc((size_t)(vl + 1));
    memcpy(*value, buf, vl);
    (*value)[vl] = '\0';
    *vallen = vl;
    return NVDB_OK;
}

nvdb_errcode_t nvdb_kv_put(NVDBEngine *engine,
                            const char *key, const char *value) {
    return (nvdb_errcode_t)btree_insert(engine->btree,
                                         key, (uint16_t)strlen(key),
                                         value, (uint16_t)strlen(value));
}

nvdb_errcode_t nvdb_kv_del(NVDBEngine *engine, const char *key) {
    return (nvdb_errcode_t)btree_delete(engine->btree,
                                         key, (uint16_t)strlen(key));
}
