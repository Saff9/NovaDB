/*
 * wal.h — Write-Ahead Log interface
 */
#ifndef NOVDB_WAL_H
#define NOVDB_WAL_H

#include "novadb/types.h"

/* Forward */
struct NVDBBTree;

/* ── Lifecycle ────────────────────────────────────────────────── */
NVDBWAL *wal_open(const char *path);
void      wal_close(NVDBWAL *wal);

/* ── Mutation logging ─────────────────────────────────────────── */
int wal_append(NVDBWAL *wal, nvdb_walop_t op, txnid_t txn_id,
               const void *key, uint16_t keylen,
               const void *value, uint32_t vallen);

/* ── Durability ───────────────────────────────────────────────── */
int wal_sync(NVDBWAL *wal);
int wal_truncate(NVDBWAL *wal);

/* ── Recovery ─────────────────────────────────────────────────── */
int wal_recover(NVDBWAL *wal, struct NVDBBTree *tree);

#endif /* NOVDB_WAL_H */
