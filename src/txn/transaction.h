/*
 * transaction.h — Transaction manager interface
 */
#ifndef NOVDB_TRANSACTION_H
#define NOVDB_TRANSACTION_H

#include "novadb/types.h"

/* ── Transaction isolation levels ─────────────────────────────── */
typedef enum {
    NVDB_ISO_READ_COMMITTED  = 0,
    NVDB_ISO_SNAPSHOT        = 1,
    NVDB_ISO_SERIALIZABLE    = 2,
} nvdb_txniso_t;

/* ── Transaction states ───────────────────────────────────────── */
typedef enum {
    NVDB_TXN_ACTIVE     = 0,
    NVDB_TXN_COMMITTING = 1,
    NVDB_TXN_COMMITTED  = 2,
    NVDB_TXN_ABORTED    = 3,
} nvdb_txnstate_t;

/* Forward */
typedef struct NVDBEngine NVDBEngine;

/* ── Manager lifecycle ────────────────────────────────────────── */
NVDBTxnMgr *txnmgr_create(NVDBEngine *engine);
void        txnmgr_destroy(NVDBTxnMgr *mgr);

/* ── Transaction lifecycle ────────────────────────────────────── */
NVDBTransaction *txnmgr_begin(NVDBTxnMgr *mgr, nvdb_txniso_t iso);
int              txnmgr_commit(NVDBTransaction *txn);
void             txnmgr_rollback(NVDBTransaction *txn);

/* ── Write-set operations ─────────────────────────────────────── */
int txn_put(NVDBTransaction *txn, const void *key, uint16_t keylen,
            const void *value, uint32_t vallen);
int txn_delete(NVDBTransaction *txn, const void *key, uint16_t keylen);

#endif /* NOVDB_TRANSACTION_H */
