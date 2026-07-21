/*
 * buffer.h — Buffer pool interface
 */
#ifndef NOVDB_BUFFER_H
#define NOVDB_BUFFER_H

#include "novadb/types.h"
#include "page.h"

/* ── Lifecycle ────────────────────────────────────────────────── */
NVDBBufferPool *bp_create(const char *db_path);
void            bp_destroy(NVDBBufferPool *bp);

/* ── Page fetch / release ─────────────────────────────────────── */
NVDBSlottedPage *bp_fetch(NVDBBufferPool *bp, pgno_t pageno);
void             bp_unpin(NVDBBufferPool *bp, NVDBSlottedPage *page);
void             bp_mark_dirty(NVDBBufferPool *bp, NVDBSlottedPage *page);

/* ── Flush ────────────────────────────────────────────────────── */
int bp_flush(NVDBBufferPool *bp, NVDBSlottedPage *page);
int bp_flush_all(NVDBBufferPool *bp);

/* ── Stats ────────────────────────────────────────────────────── */
void bp_stats(const NVDBBufferPool *bp, uint64_t *hits, uint64_t *misses);

/* ── Accessors ────────────────────────────────────────────────── */
int bp_fd(const NVDBBufferPool *bp);

#endif /* NOVDB_BUFFER_H */
