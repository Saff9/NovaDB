/*
 * types.h — Core type definitions for NovaDB
 * C99-compatible, no GNU extensions required.
 */
#ifndef NOVADB_TYPES_H
#define NOVADB_TYPES_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <sys/types.h>

/* Pull in platform/build config */
#include "nvdb_config.h"

/* ── Compiler / platform helpers ──────────────────────────────── */
#if defined(__GNUC__) || defined(__clang__)
#  define NVDB_NORETURN    __attribute__((noreturn))
#  define NVDB_MALLOC      __attribute__((malloc))
#  define NVDB_UNUSED      __attribute__((unused))
#  define NVDB_ALIGNED(n)  __attribute__((aligned(n)))
#  define NVDB_LIKELY(x)   __builtin_expect(!!(x), 1)
#  define NVDB_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#  define NVDB_NORETURN
#  define NVDB_MALLOC
#  define NVDB_UNUSED
#  define NVDB_ALIGNED(n)
#  define NVDB_LIKELY(x)   (x)
#  define NVDB_UNLIKELY(x) (x)
#endif

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
#  define NVDB_STATIC_ASSERT(c, m) _Static_assert(c, m)
#else
#  define NVDB_STATIC_ASSERT(c, m)
#endif

/* ── Page identifiers ─────────────────────────────────────────── */
typedef uint64_t pgno_t;
typedef uint32_t txnid_t;
typedef uint64_t lsn_t;
typedef uint32_t slotid_t;
typedef uint16_t colid_t;

#define PGNO_INVALID   ((pgno_t)0)
#define TXNID_INVALID  ((txnid_t)0)
#define LSN_INVALID    ((lsn_t)0)

/* ── Opaque handles ───────────────────────────────────────────── */
typedef struct NVDBEngine      NVDBEngine;
typedef struct NVDBServer      NVDBServer;
typedef struct NVDBTransaction NVDBTransaction;
typedef struct NVDBResultSet   NVDBResultSet;
typedef struct NVDBArena       NVDBArena;
typedef struct NVDBSlab        NVDBSlab;

/* ── Error codes ──────────────────────────────────────────────── */
typedef enum {
    NVDB_OK               =  0,
    NVDB_ERR_IO           = -1,
    NVDB_ERR_NOMEM        = -2,
    NVDB_ERR_CORRUPT      = -3,
    NVDB_ERR_NOT_FOUND    = -4,
    NVDB_ERR_DUPLICATE    = -5,
    NVDB_ERR_FULL         = -6,
    NVDB_ERR_PROTOCOL     = -7,
    NVDB_ERR_PARSE        = -8,
    NVDB_ERR_EXEC         = -9,
    NVDB_ERR_TXN_ABORTED  = -10,
    NVDB_ERR_TXN_CONFLICT = -11,
    NVDB_ERR_TXN_EXPIRED  = -12,
    NVDB_ERR_CONN_LIMIT   = -13,
    NVDB_ERR_TIMEOUT      = -14,
    NVDB_ERR_NOT_IMPL     = -15,
    NVDB_ERR_INTERNAL     = -99,
} nvdb_errcode_t;

/* ── Value types ──────────────────────────────────────────────── */
typedef enum {
    NVDB_TYPE_NULL    = 0,
    NVDB_TYPE_INT64   = 1,
    NVDB_TYPE_FLOAT64 = 2,
    NVDB_TYPE_STRING  = 3,
    NVDB_TYPE_BLOB    = 4,
    NVDB_TYPE_BOOL    = 5,
} nvdb_valtype_t;

/*
 * Flat tagged value — no union, fully C99 compatible.
 * String data is owned by this struct; caller must
 * nvdb_val_free() before overwriting.
 */
typedef struct {
    nvdb_valtype_t type;
    int64_t   i64;
    double    f64;
    bool      bval;
    char     *str_val;
    uint32_t  str_len;
} NVDBValue;

/* Initialisers */
static inline void nvdb_val_set_null(NVDBValue *v) {
    v->type    = NVDB_TYPE_NULL;
    v->i64     = 0;
}

static inline void nvdb_val_set_i64(NVDBValue *v, int64_t n) {
    v->type    = NVDB_TYPE_INT64;
    v->i64     = n;
}

static inline void nvdb_val_set_f64(NVDBValue *v, double d) {
    v->type    = NVDB_TYPE_FLOAT64;
    v->f64     = d;
}

static inline void nvdb_val_set_bool(NVDBValue *v, bool b) {
    v->type    = NVDB_TYPE_BOOL;
    v->bval    = b;
}

static inline void nvdb_val_set_str(NVDBValue *v, char *s, uint32_t len) {
    v->type    = NVDB_TYPE_STRING;
    v->str_val = s;
    v->str_len = len;
}

static inline void nvdb_val_free(NVDBValue *v) {
    if ((v->type == NVDB_TYPE_STRING || v->type == NVDB_TYPE_BLOB)
        && v->str_val) {
        free(v->str_val);
        v->str_val = NULL;
        v->str_len = 0;
    }
    v->type = NVDB_TYPE_NULL;
}

/* ── Page constants ───────────────────────────────────────────── */
#define NVDB_PAGE_SIZE        8192
#define NVDB_PAGE_HEADER_SIZE 40
#define NVDB_MAX_KEY_SIZE     2048

typedef enum {
    NVDB_PAGE_META       = 0,
    NVDB_PAGE_BTREE_LEAF = 1,
    NVDB_PAGE_BTREE_INT  = 2,
    NVDB_PAGE_OVERFLOW   = 3,
} nvdb_pagetype_t;

typedef struct NVDB_ALIGNED(8) {
    uint32_t  magic;
    uint8_t   page_type;
    uint8_t   _pad0;
    uint16_t  flags;
    pgno_t    self;
    pgno_t    parent;
    pgno_t    right_sibling;
    uint16_t  nslots;
    uint16_t  free_offset;
    uint16_t  free_length;
    uint16_t  _pad1;
} NVDBPageHeader;

NVDB_STATIC_ASSERT(sizeof(NVDBPageHeader) == 40, "PageHeader must be 40 bytes");

#define NVDB_PAGE_MAGIC  0x4E564442

/* ── Slot entry ───────────────────────────────────────────────── */
typedef struct NVDB_ALIGNED(4) {
    uint16_t offset;
    uint16_t key_length;
    uint16_t value_length;
    uint8_t  flags;
    uint8_t  _pad;
} NVDBSlotEntry;

#define NVDB_SLOT_ENTRY_SIZE 8

/* ── WAL operations ───────────────────────────────────────────── */
typedef enum {
    NVDB_WAL_INSERT     = 1,
    NVDB_WAL_DELETE     = 2,
    NVDB_WAL_UPDATE     = 3,
    NVDB_WAL_COMMIT     = 4,
    NVDB_WAL_ABORT      = 5,
    NVDB_WAL_CHECKPOINT = 6,
} nvdb_walop_t;

typedef struct NVDB_ALIGNED(8) {
    uint32_t  crc32;
    uint32_t  total_len;
    lsn_t     lsn;
    txnid_t   txn_id;
    uint8_t   op;
    uint8_t   _pad[3];
} NVDBWALRecord;

#define NVDB_WAL_RECORD_HDR_SIZE 24

/* ── Internal type forwards ───────────────────────────────────── */
typedef struct NVDBSlottedPage NVDBSlottedPage;
typedef struct NVDBBufferPool  NVDBBufferPool;
typedef struct NVDBBTree       NVDBBTree;
typedef struct NVDBWAL         NVDBWAL;
typedef struct NVDBTxnMgr      NVDBTxnMgr;

/* ── Public ResultSet ─────────────────────────────────────────── */
struct NVDBResultSet {
    int         ncols;
    char      **col_names;
    int         nrows;
    NVDBValue  **rows;
    char       *message;
    bool        error;
    char       *error_msg;
};

/* ── Statistics ───────────────────────────────────────────────── */
typedef struct {
    uint64_t pages_read;
    uint64_t pages_written;
    uint64_t wal_bytes_written;
    uint64_t transactions_committed;
    uint64_t transactions_rolled_back;
    uint64_t buffer_pool_hits;
    uint64_t buffer_pool_misses;
} NVDBStats;

#endif /* NOVADB_TYPES_H */
