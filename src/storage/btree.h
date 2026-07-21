/*
 * btree.h — B+Tree interface
 */
#ifndef NOVDB_BTREE_H
#define NOVDB_BTREE_H

#include "novadb/types.h"
#include "buffer.h"

/* ── Lifecycle ────────────────────────────────────────────────── */
NVDBBTree  *btree_open(NVDBBufferPool *bp);
void        btree_close(NVDBBTree *tree);

/* ── CRUD ─────────────────────────────────────────────────────── */
int btree_search(NVDBBTree *tree,
                 const void *key, uint16_t keylen,
                 void *value_out, uint16_t *vallen_out,
                 uint16_t max_vallen);

int btree_insert(NVDBBTree *tree,
                 const void *key, uint16_t keylen,
                 const void *value, uint16_t vallen);

int btree_delete(NVDBBTree *tree,
                 const void *key, uint16_t keylen);

/* ── Range scan ───────────────────────────────────────────────── */
int btree_scan(NVDBBTree *tree,
               const void *start_key, uint16_t start_keylen,
               const void *end_key,   uint16_t end_keylen,
               int (*callback)(const void *key, uint16_t keylen,
                               const void *value, uint16_t vallen,
                               void *arg),
               void *arg);

/* ── Accessors ────────────────────────────────────────────────── */
pgno_t   btree_root(const NVDBBTree *tree);
pgno_t   btree_next_page(const NVDBBTree *tree);
uint64_t btree_key_count(const NVDBBTree *tree);

/* ── Helpers ──────────────────────────────────────────────────── */
void read_slot_internal(const NVDBSlottedPage *page, uint16_t idx,
                         NVDBSlotEntry *e);

#endif /* NOVDB_BTREE_H */
