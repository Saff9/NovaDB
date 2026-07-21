/*
 * page.c — Slotted page management
 *
 * Every page in NovaDB uses the slotted-page layout familiar from
 * PostgreSQL's bufpage.h and SQLite's btreeInt.h:
 *
 *   ┌─────────────────────────────────────────┐
 *   │ PageHeader (32 bytes)                    │  ← fixed
 *   ├─────────────────────────────────────────┤
 *   │ Slot 0 │ Slot 1 │ ... │ Slot N-1        │  ← grows downward
 *   │  8 B   │  8 B   │     │  8 B            │
 *   ├─────────────────────────────────────────┤
 *   │                 ... free space ...       │
 *   ├─────────────────────────────────────────┤
 *   │ Data N-1 │ ... │ Data 1 │ Data 0        │  ← grows upward
 *   └─────────────────────────────────────────┘
 *
 * Slot directory grows downward from the header. Record data
 * grows upward from the page end.  A page is full when the
 * two regions meet.
 *
 * All multi-byte integers on disk are little-endian.
 */

#include <string.h>
#include "page.h"
#include "memory.h"

/* ── Byte-order helpers (explicit, no surprises) ──────────────── */

static inline uint16_t rd16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static inline void wr16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v);
    p[1] = (uint8_t)(v >> 8);
}

static inline uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0]       | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline void wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v);        p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);  p[3] = (uint8_t)(v >> 24);
}

static inline uint64_t rd64(const uint8_t *p) {
    return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32);
}

static inline void wr64(uint8_t *p, uint64_t v) {
    wr32(p, (uint32_t)(v));
    wr32(p + 4, (uint32_t)(v >> 32));
}

/* ── Page layout offsets (from start of byte buffer) ──────────── */

#define OFF_MAGIC        0
#define OFF_PAGE_TYPE    4
#define OFF_FLAGS        6
#define OFF_SELF         8
#define OFF_PARENT      16
#define OFF_RIGHT_SIB   24
#define OFF_NSLOTS      32
#define OFF_FREE_OFF    34
#define OFF_FREE_LEN    36
/* slot directory starts at byte 40 (NVDB_PAGE_HEADER_SIZE + 8 bytes reserved) */
#define OFF_SLOTS       40

/* ── Decode header from raw bytes ─────────────────────────────── */

void page_decode_header(const uint8_t *raw, NVDBPageHeader *hdr) {
    hdr->magic        = rd32(raw + OFF_MAGIC);
    hdr->page_type    = raw[OFF_PAGE_TYPE];
    hdr->flags        = rd16(raw + OFF_FLAGS);
    hdr->self         = rd64(raw + OFF_SELF);
    hdr->parent       = rd64(raw + OFF_PARENT);
    hdr->right_sibling = rd64(raw + OFF_RIGHT_SIB);
    hdr->nslots       = rd16(raw + OFF_NSLOTS);
    hdr->free_offset  = rd16(raw + OFF_FREE_OFF);
    hdr->free_length  = rd16(raw + OFF_FREE_LEN);
}

void page_encode_header(uint8_t *raw, const NVDBPageHeader *hdr) {
    wr32(raw + OFF_MAGIC, hdr->magic);
    raw[OFF_PAGE_TYPE] = hdr->page_type;
    wr16(raw + OFF_FLAGS, hdr->flags);
    wr64(raw + OFF_SELF, hdr->self);
    wr64(raw + OFF_PARENT, hdr->parent);
    wr64(raw + OFF_RIGHT_SIB, hdr->right_sibling);
    wr16(raw + OFF_NSLOTS, hdr->nslots);
    wr16(raw + OFF_FREE_OFF, hdr->free_offset);
    wr16(raw + OFF_FREE_LEN, hdr->free_length);
}

/* ── Initialise a blank page ──────────────────────────────────── */

void page_init(NVDBSlottedPage *page, pgno_t pageno, nvdb_pagetype_t type) {
    memset(page->data, 0, NVDB_PAGE_SIZE);

    NVDBPageHeader hdr = {
        .magic        = NVDB_PAGE_MAGIC,
        .page_type    = (uint8_t)type,
        .flags        = 0,
        .self         = pageno,
        .parent       = PGNO_INVALID,
        .right_sibling = PGNO_INVALID,
        .nslots       = 0,
        .free_offset  = OFF_SLOTS,
        .free_length  = (uint16_t)(NVDB_PAGE_SIZE - OFF_SLOTS),
    };
    page_encode_header(page->data, &hdr);
    page->pageno = pageno;
    page->dirty  = true;
}

/* ── Slot access ──────────────────────────────────────────────── */

static uint16_t slot_pos(uint16_t idx) {
    return OFF_SLOTS + idx * NVDB_SLOT_ENTRY_SIZE;
}

static void read_slot(const uint8_t *page, uint16_t idx, NVDBSlotEntry *e) {
    uint16_t pos = slot_pos(idx);
    e->offset       = rd16(page + pos);
    e->key_length   = rd16(page + pos + 2);
    e->value_length = rd16(page + pos + 4);
    e->flags        = page[pos + 6];
}

static void write_slot(uint8_t *page, uint16_t idx, const NVDBSlotEntry *e) {
    uint16_t pos = slot_pos(idx);
    wr16(page + pos,     e->offset);
    wr16(page + pos + 2, e->key_length);
    wr16(page + pos + 4, e->value_length);
    page[pos + 6] = e->flags;
}

/* ── High-level slot operations ───────────────────────────────── */

int page_insert(NVDBSlottedPage *page,
                const void *key, uint16_t keylen,
                const void *value, uint16_t vallen) {
    if (keylen == 0) return NVDB_ERR_INTERNAL;

    uint16_t needed = keylen + vallen;
    NVDBPageHeader hdr;
    page_decode_header(page->data, &hdr);

    /* Check for duplicate key in leaf pages */
    if (hdr.page_type == NVDB_PAGE_BTREE_LEAF) {
        for (uint16_t i = 0; i < hdr.nslots; i++) {
            NVDBSlotEntry e;
            read_slot(page->data, i, &e);
            if (e.key_length == keylen &&
                memcmp(page->data + e.offset, key, keylen) == 0) {
                return NVDB_ERR_DUPLICATE;
            }
        }
    }

    /* Need space for both slot entry and data */
    if (needed + NVDB_SLOT_ENTRY_SIZE > hdr.free_length) {
        return NVDB_ERR_FULL;
    }

    /* Data goes at (free_offset + free_length - needed), growing upward */
    uint16_t data_off = hdr.free_offset + hdr.free_length - needed;

    /* Write data */
    memcpy(page->data + data_off, key, keylen);
    if (vallen > 0) {
        memcpy(page->data + data_off + keylen, value, vallen);
    }

    /* Write slot entry */
    NVDBSlotEntry e = {
        .offset       = data_off,
        .key_length   = keylen,
        .value_length = vallen,
        .flags        = 0,
    };
    write_slot(page->data, hdr.nslots, &e);

    /* Update header */
    hdr.nslots++;
    hdr.free_length -= (needed + NVDB_SLOT_ENTRY_SIZE);
    page_encode_header(page->data, &hdr);
    page->dirty = true;

    return NVDB_OK;
}

int page_update(NVDBSlottedPage *page,
                const void *key, uint16_t keylen,
                const void *new_value, uint16_t new_vallen) {
    NVDBPageHeader hdr;
    page_decode_header(page->data, &hdr);

    for (uint16_t i = 0; i < hdr.nslots; i++) {
        NVDBSlotEntry e;
        read_slot(page->data, i, &e);

        if (e.key_length == keylen &&
            memcmp(page->data + e.offset, key, keylen) == 0) {

            /* If new value fits in same space, overwrite in place */
            if (new_vallen <= e.value_length) {
                memcpy(page->data + e.offset + keylen, new_value, new_vallen);
                e.value_length = new_vallen;
                write_slot(page->data, i, &e);
                page->dirty = true;
                return NVDB_OK;
            }

            /* Otherwise, remove old slot and re-insert */
            page_delete_slot(page, i);
            return page_insert(page, key, keylen, new_value, new_vallen);
        }
    }
    return NVDB_ERR_NOT_FOUND;
}

int page_delete_slot(NVDBSlottedPage *page, uint16_t idx) {
    NVDBPageHeader hdr;
    page_decode_header(page->data, &hdr);

    if (idx >= hdr.nslots) return NVDB_ERR_NOT_FOUND;

    /* Shift slot entries down by one */
    for (uint16_t i = idx; i < hdr.nslots - 1; i++) {
        NVDBSlotEntry e;
        read_slot(page->data, (uint16_t)(i + 1), &e);
        write_slot(page->data, i, &e);
    }

    hdr.nslots--;
    /* We don't reclaim data space — that's a compaction concern */
    page_encode_header(page->data, &hdr);
    page->dirty = true;
    return NVDB_OK;
}

int page_find(const NVDBSlottedPage *page,
              const void *key, uint16_t keylen,
              NVDBSlotEntry *out, uint16_t *out_idx) {
    NVDBPageHeader hdr;
    page_decode_header(page->data, &hdr);

    for (uint16_t i = 0; i < hdr.nslots; i++) {
        NVDBSlotEntry e;
        read_slot(page->data, i, &e);

        if (e.key_length == keylen &&
            memcmp(page->data + e.offset, key, keylen) == 0) {
            if (out)      memcpy(out, &e, sizeof(*out));
            if (out_idx)  *out_idx = i;
            return NVDB_OK;
        }
    }
    return NVDB_ERR_NOT_FOUND;
}

bool page_is_full(const NVDBSlottedPage *page, uint16_t needed) {
    NVDBPageHeader hdr;
    page_decode_header(page->data, &hdr);
    return (needed + NVDB_SLOT_ENTRY_SIZE > hdr.free_length);
}

uint16_t page_freespace(const NVDBSlottedPage *page) {
    NVDBPageHeader hdr;
    page_decode_header(page->data, &hdr);
    if (hdr.free_length > NVDB_SLOT_ENTRY_SIZE)
        return hdr.free_length - NVDB_SLOT_ENTRY_SIZE;
    return 0;
}

/* ── Binary search within a page (keys are sorted by slot order) ─ */

int page_bsearch(const NVDBSlottedPage *page,
                 const void *key, uint16_t keylen,
                 uint16_t *out_idx) {
    NVDBPageHeader hdr;
    page_decode_header(page->data, &hdr);

    if (hdr.nslots == 0) {
        *out_idx = 0;
        return NVDB_ERR_NOT_FOUND;
    }

    int lo = 0, hi = (int)hdr.nslots - 1;

    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        NVDBSlotEntry e;
        read_slot(page->data, (uint16_t)mid, &e);

        uint16_t cmp_len = (keylen < e.key_length) ? keylen : e.key_length;
        int cmp = memcmp(key, page->data + e.offset, cmp_len);

        if (cmp == 0 && keylen == e.key_length) {
            *out_idx = (uint16_t)mid;
            return NVDB_OK;  /* exact match */
        }
        if (cmp < 0 || (cmp == 0 && keylen < e.key_length)) {
            hi = mid - 1;
        } else {
            lo = mid + 1;
        }
    }

    *out_idx = (uint16_t)lo;  /* insertion point */
    return NVDB_ERR_NOT_FOUND;
}

/* ── Public slot access wrappers ──────────────────────────────── */

void page_read_slot_entry(const uint8_t *raw, uint16_t idx, NVDBSlotEntry *e) {
    read_slot(raw, idx, e);
}

void page_write_slot_entry(uint8_t *raw, uint16_t idx, const NVDBSlotEntry *e) {
    write_slot(raw, idx, e);
}

/* ── Validate page integrity ──────────────────────────────────── */

bool page_verify(const NVDBSlottedPage *page) {
    NVDBPageHeader hdr;
    page_decode_header(page->data, &hdr);

    if (hdr.magic != NVDB_PAGE_MAGIC)
        return false;
    if (hdr.page_type > NVDB_PAGE_OVERFLOW)
        return false;
    if (hdr.self == PGNO_INVALID)
        return false;

    /* Check slot offsets are within bounds */
    for (uint16_t i = 0; i < hdr.nslots; i++) {
        NVDBSlotEntry e;
        read_slot(page->data, i, &e);
        if (e.offset + e.key_length + e.value_length > NVDB_PAGE_SIZE)
            return false;
    }

    return true;
}
