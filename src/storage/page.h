/*
 * page.h — Slotted page interface (internal)
 */
#ifndef NOVDB_PAGE_H
#define NOVDB_PAGE_H

#include "novadb/types.h"

/* ── In-memory page representation ────────────────────────────── */

/*
 * A page is 8KB of raw bytes plus a few bookkeeping fields.
 * The data[] array is the on-disk format; the header is
 * decoded/encoded on demand rather than stored separately.
 */
typedef struct {
    uint8_t  data[NVDB_PAGE_SIZE];
    pgno_t   pageno;
    bool     dirty;
    int      pin_count;
} NVDBSlottedPage;

/* ── Header encode/decode ─────────────────────────────────────── */
void page_decode_header(const uint8_t *raw, NVDBPageHeader *hdr);
void page_encode_header(uint8_t *raw, const NVDBPageHeader *hdr);

/* ── Page lifecycle ───────────────────────────────────────────── */
void page_init(NVDBSlottedPage *page, pgno_t pageno, nvdb_pagetype_t type);

/* ── Slot operations ──────────────────────────────────────────── */
int  page_insert(NVDBSlottedPage *page,
                 const void *key, uint16_t keylen,
                 const void *value, uint16_t vallen);

int  page_update(NVDBSlottedPage *page,
                 const void *key, uint16_t keylen,
                 const void *new_value, uint16_t new_vallen);

int  page_delete_slot(NVDBSlottedPage *page, uint16_t idx);

int  page_find(const NVDBSlottedPage *page,
               const void *key, uint16_t keylen,
               NVDBSlotEntry *out, uint16_t *out_idx);

int  page_bsearch(const NVDBSlottedPage *page,
                  const void *key, uint16_t keylen,
                  uint16_t *out_idx);

/* ── Space queries ────────────────────────────────────────────── */
bool     page_is_full(const NVDBSlottedPage *page, uint16_t needed);
uint16_t page_freespace(const NVDBSlottedPage *page);

/* ── Raw slot access (for bulk operations) ────────────────────── */
void page_read_slot_entry(const uint8_t *raw, uint16_t idx, NVDBSlotEntry *e);
void page_write_slot_entry(uint8_t *raw, uint16_t idx, const NVDBSlotEntry *e);

/* ── Integrity ────────────────────────────────────────────────── */
bool page_verify(const NVDBSlottedPage *page);

#endif /* NOVDB_PAGE_H */
