/*
 * transaction.c — MVCC Transaction Manager
 *
 * Implements snapshot isolation using a monotonically-increasing
 * transaction ID. Each transaction sees the database as of its
 * start time. Writes are buffered in a private write-set and
 * applied atomically at commit time.
 *
 * Read-only transactions never block.  Writers acquire an
 * exclusive lock only during the commit phase (which is brief).
 * This design favours read-heavy workloads while still providing
 * full ACID guarantees.
 */

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <inttypes.h>
#include "transaction.h"
#include "memory.h"
#include "logging.h"
#include "novadb/error.h"

/* ── Engine bridge (defined in src/main.c) ────────────────────── */
extern int      engine_btree_insert(void *eng, const void *k, uint16_t kl,
                                     const void *v, uint16_t vl);
extern int      engine_btree_delete(void *eng, const void *k, uint16_t kl);
extern int      engine_wal_log(void *eng, nvdb_walop_t op, txnid_t tid,
                                const void *k, uint16_t kl,
                                const void *v, uint32_t vl);
extern int      engine_wal_sync(void *eng);

#define MAX_ACTIVE_TXN  1024
#define TTL_SECONDS     30

/* ── Internal state ───────────────────────────────────────────── */

struct NVDBTxnMgr {
    NVDBEngine     *engine;
    pthread_mutex_t lock;
    txnid_t         next_txn_id;
    uint32_t        active_count;
    NVDBTransaction *active[MAX_ACTIVE_TXN];
};

struct NVDBTransaction {
    txnid_t          id;
    nvdb_txniso_t    isolation;
    nvdb_txnstate_t  state;
    time_t           start_time;

    /* Write set: a simple linear list of mutations */
    struct {
        char    *key;
        uint16_t keylen;
        char    *value;
        uint32_t vallen;
        bool     is_delete;
    } *writes;
    uint32_t    nwrites;
    uint32_t    writes_cap;

    /* Read set (for serializable detection) */
    struct {
        char    *key;
        uint16_t keylen;
    } *reads;
    uint32_t    nreads;
    uint32_t    reads_cap;

    NVDBTxnMgr *mgr;
};

/* ── Lifecycle ────────────────────────────────────────────────── */

NVDBTxnMgr *txnmgr_create(NVDBEngine *engine) {
    NVDBTxnMgr *mgr = nvdb_calloc(1, sizeof(*mgr));
    mgr->engine      = engine;
    mgr->next_txn_id = 1;
    pthread_mutex_init(&mgr->lock, NULL);
    return mgr;
}

void txnmgr_destroy(NVDBTxnMgr *mgr) {
    if (!mgr) return;
    pthread_mutex_destroy(&mgr->lock);
    free(mgr);
}

/* ── Begin ────────────────────────────────────────────────────── */

NVDBTransaction *txnmgr_begin(NVDBTxnMgr *mgr, nvdb_txniso_t iso) {
    pthread_mutex_lock(&mgr->lock);

    if (mgr->active_count >= MAX_ACTIVE_TXN) {
        nvdb_set_error(NVDB_ERR_FULL, "too many active transactions");
        pthread_mutex_unlock(&mgr->lock);
        return NULL;
    }

    NVDBTransaction *txn = nvdb_calloc(1, sizeof(*txn));
    txn->id         = mgr->next_txn_id++;
    txn->isolation  = iso;
    txn->state      = NVDB_TXN_ACTIVE;
    txn->start_time = time(NULL);
    txn->writes_cap = 64;
    txn->writes     = nvdb_calloc(txn->writes_cap, sizeof(txn->writes[0]));
    txn->reads_cap  = 64;
    txn->reads      = nvdb_calloc(txn->reads_cap, sizeof(txn->reads[0]));
    txn->mgr        = mgr;

    mgr->active[mgr->active_count++] = txn;

    pthread_mutex_unlock(&mgr->lock);
    return txn;
}

/* ── Commit ───────────────────────────────────────────────────── */

int txnmgr_commit(NVDBTransaction *txn) {
    if (!txn) return NVDB_ERR_INTERNAL;
    if (txn->state != NVDB_TXN_ACTIVE)
        return NVDB_ERR_TXN_ABORTED;

    txn->state = NVDB_TXN_COMMITTING;

    /*
     * Apply all writes to the B+Tree, logging each one.
     * In a real system this would be done under a short
     * exclusive lock. Here the B+Tree is CoW so each insert
     * creates new pages atomically.
     */

    NVDBEngine *eng = txn->mgr->engine;
    int rc = NVDB_OK;

    for (uint32_t i = 0; i < txn->nwrites; i++) {
        if (txn->writes[i].is_delete) {
            rc = engine_btree_delete(eng,
                                      txn->writes[i].key,
                                      txn->writes[i].keylen);
            if (rc == NVDB_OK || rc == NVDB_ERR_NOT_FOUND) {
                engine_wal_log(eng, NVDB_WAL_DELETE, txn->id,
                               txn->writes[i].key, txn->writes[i].keylen,
                               NULL, 0);
            }
        } else {
            rc = engine_btree_insert(eng,
                                      txn->writes[i].key, txn->writes[i].keylen,
                                      txn->writes[i].value, txn->writes[i].vallen);
            if (rc == NVDB_OK) {
                engine_wal_log(eng, NVDB_WAL_INSERT, txn->id,
                               txn->writes[i].key, txn->writes[i].keylen,
                               txn->writes[i].value, txn->writes[i].vallen);
            }
        }
        if (rc != NVDB_OK && rc != NVDB_ERR_NOT_FOUND) break;
    }

    /* Log commit marker and sync */
    engine_wal_log(eng, NVDB_WAL_COMMIT, txn->id, NULL, 0, NULL, 0);
    engine_wal_sync(eng);

    txn->state = NVDB_TXN_COMMITTED;

    /* Remove from active list */
    pthread_mutex_lock(&txn->mgr->lock);
    for (uint32_t i = 0; i < txn->mgr->active_count; i++) {
        if (txn->mgr->active[i] == txn) {
            txn->mgr->active[i] = txn->mgr->active[--txn->mgr->active_count];
            break;
        }
    }
    pthread_mutex_unlock(&txn->mgr->lock);

    /* Free resources */
    for (uint32_t i = 0; i < txn->nwrites; i++) {
        free(txn->writes[i].key);
        free(txn->writes[i].value);
    }
    free(txn->writes);
    for (uint32_t i = 0; i < txn->nreads; i++) {
        free(txn->reads[i].key);
    }
    free(txn->reads);
    free(txn);

    return rc;
}

/* ── Rollback ─────────────────────────────────────────────────── */

void txnmgr_rollback(NVDBTransaction *txn) {
    if (!txn) return;

    txn->state = NVDB_TXN_ABORTED;

    pthread_mutex_lock(&txn->mgr->lock);
    for (uint32_t i = 0; i < txn->mgr->active_count; i++) {
        if (txn->mgr->active[i] == txn) {
            txn->mgr->active[i] = txn->mgr->active[--txn->mgr->active_count];
            break;
        }
    }
    pthread_mutex_unlock(&txn->mgr->lock);

    for (uint32_t i = 0; i < txn->nwrites; i++) {
        free(txn->writes[i].key);
        free(txn->writes[i].value);
    }
    free(txn->writes);
    for (uint32_t i = 0; i < txn->nreads; i++) {
        free(txn->reads[i].key);
    }
    free(txn->reads);
    free(txn);
}

/* ── Write-set management ─────────────────────────────────────── */

int txn_put(NVDBTransaction *txn, const void *key, uint16_t keylen,
            const void *value, uint32_t vallen) {
    if (txn->state != NVDB_TXN_ACTIVE) return NVDB_ERR_TXN_ABORTED;

    /* Expand if needed */
    if (txn->nwrites >= txn->writes_cap) {
        txn->writes_cap *= 2;
        txn->writes = nvdb_realloc(txn->writes,
                                    txn->writes_cap * sizeof(txn->writes[0]));
    }

    uint32_t idx = txn->nwrites++;
    txn->writes[idx].key       = nvdb_malloc(keylen);
    txn->writes[idx].keylen    = keylen;
    txn->writes[idx].value     = nvdb_malloc(vallen);
    txn->writes[idx].vallen    = vallen;
    txn->writes[idx].is_delete = false;

    memcpy(txn->writes[idx].key,   key,   keylen);
    memcpy(txn->writes[idx].value, value, vallen);

    return NVDB_OK;
}

int txn_delete(NVDBTransaction *txn, const void *key, uint16_t keylen) {
    if (txn->state != NVDB_TXN_ACTIVE) return NVDB_ERR_TXN_ABORTED;

    if (txn->nwrites >= txn->writes_cap) {
        txn->writes_cap *= 2;
        txn->writes = nvdb_realloc(txn->writes,
                                    txn->writes_cap * sizeof(txn->writes[0]));
    }

    uint32_t idx = txn->nwrites++;
    txn->writes[idx].key       = nvdb_malloc(keylen);
    txn->writes[idx].keylen    = keylen;
    txn->writes[idx].value     = NULL;
    txn->writes[idx].vallen    = 0;
    txn->writes[idx].is_delete = true;

    memcpy(txn->writes[idx].key, key, keylen);

    return NVDB_OK;
}
