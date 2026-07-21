/*
 * wal.c — Write-Ahead Log with CRC-32C integrity checking
 *
 * Design (inspired by PostgreSQL's xlog and LevelDB's WAL):
 *   - Sequential append-only log file
 *   - Each record: [CRC32C:4][total_len:4][LSN:8][txn_id:4][op:1][kl:2][vl:4][key][value]
 *   - Records are CRC-protected; corruption is detected on recovery
 *   - fsync after each commit for durability
 *   - Checkpoint truncates the log after all dirty pages are flushed
 *
 * Recovery: on startup, the WAL is scanned from the beginning.
 * All committed transactions are replayed against the B+Tree.
 * Corrupt records (bad CRC) truncate the log at that point.
 */

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <inttypes.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include "wal.h"
#include "btree.h"
#include "buffer.h"
#include "memory.h"
#include "logging.h"

/* ── CRC-32C (Castagnoli) ─────────────────────────────────────── */
/* Polynomial: 0x1EDC6F41 (iSCSI/SCSI CRC)                        */

static uint32_t crc32c_table[256];
static int      crc32c_ready = 0;

static void crc32c_init(void) {
    if (crc32c_ready) return;
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t crc = i;
        for (int j = 0; j < 8; j++) {
            crc = (crc & 1) ? (crc >> 1) ^ 0x82F63B78UL : (crc >> 1);
        }
        crc32c_table[i] = crc;
    }
    crc32c_ready = 1;
}

static uint32_t crc32c_compute(const void *data, size_t len) {
    crc32c_init();
    uint32_t crc = 0xFFFFFFFFUL;
    const uint8_t *p = (const uint8_t *)data;
    for (size_t i = 0; i < len; i++) {
        crc = crc32c_table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFUL;
}

/* ── WAL record wire format ───────────────────────────────────── */
/*
 * Offset  Size  Field
 * 0       4     CRC32C (over bytes 4..total_len)
 * 4       4     total_len (header + payload)
 * 8       8     LSN
 * 16      4     txn_id
 * 20      1     op code
 * 21      2     key_length
 * 23      4     value_length
 * 27      -     key (key_length bytes)
 * 27+kl   -     value (value_length bytes)
 */
#define WAL_HDR_FIXED  27  /* fixed header size before key+value */
#define WAL_CRC_OFF     0
#define WAL_LEN_OFF     4
#define WAL_LSN_OFF     8
#define WAL_TXN_OFF    16
#define WAL_OP_OFF     20
#define WAL_KLEN_OFF   21
#define WAL_VLEN_OFF   23

struct NVDBWAL {
    int         fd;
    char        path[512];
    lsn_t       next_lsn;
    uint64_t    bytes_written;
    pthread_mutex_t lock;
};

/* ── Lifecycle ────────────────────────────────────────────────── */

NVDBWAL *wal_open(const char *path) {
    NVDBWAL *wal = nvdb_calloc(1, sizeof(*wal));

    snprintf(wal->path, sizeof(wal->path), "%s/wal.log", path);

    wal->fd = open(wal->path, O_RDWR | O_CREAT | O_APPEND, 0640);
    if (wal->fd < 0) {
        nvdb_set_error(NVDB_ERR_IO, "cannot open WAL %s: %s",
                       wal->path, strerror(errno));
        free(wal);
        return NULL;
    }

    /* Determine next LSN from file size */
    off_t sz = lseek(wal->fd, 0, SEEK_END);
    wal->next_lsn = (lsn_t)(sz > 0 ? sz : 1);
    wal->bytes_written = (uint64_t)sz;

    pthread_mutex_init(&wal->lock, NULL);
    crc32c_init();

    nvdb_log_info("WAL opened: %s, next_lsn=%" PRIu64, wal->path, wal->next_lsn);
    return wal;
}

void wal_close(NVDBWAL *wal) {
    if (!wal) return;
    pthread_mutex_lock(&wal->lock);
    if (wal->fd >= 0) {
        fsync(wal->fd);
        close(wal->fd);
    }
    pthread_mutex_unlock(&wal->lock);
    pthread_mutex_destroy(&wal->lock);
    free(wal);
}

/* ── Append a record ──────────────────────────────────────────── */

int wal_append(NVDBWAL *wal, nvdb_walop_t op, txnid_t txn_id,
               const void *key, uint16_t keylen,
               const void *value, uint32_t vallen) {
    pthread_mutex_lock(&wal->lock);

    uint32_t total_len = WAL_HDR_FIXED + keylen + vallen;
    uint8_t *buf = nvdb_malloc(total_len);

    /* Zero CRC for now */
    memset(buf + WAL_CRC_OFF, 0, 4);

    /* total_len */
    memcpy(buf + WAL_LEN_OFF, &total_len, 4);

    /* LSN */
    memcpy(buf + WAL_LSN_OFF, &wal->next_lsn, 8);

    /* txn_id */
    memcpy(buf + WAL_TXN_OFF, &txn_id, 4);

    /* op */
    buf[WAL_OP_OFF] = (uint8_t)op;

    /* key_length */
    memcpy(buf + WAL_KLEN_OFF, &keylen, 2);

    /* value_length */
    memcpy(buf + WAL_VLEN_OFF, &vallen, 4);

    /* key + value — len may be 0 (commit records, tombstones) */
    if (keylen > 0 && key)
        memcpy(buf + WAL_HDR_FIXED, key, keylen);
    if (vallen > 0 && value)
        memcpy(buf + WAL_HDR_FIXED + keylen, value, vallen);

    /* Compute CRC over everything except the CRC field itself */
    uint32_t crc = crc32c_compute(buf + 4, total_len - 4);
    memcpy(buf + WAL_CRC_OFF, &crc, 4);

    /* Write to disk */
    ssize_t n = write(wal->fd, buf, total_len);
    if (n != (ssize_t)total_len) {
        nvdb_set_error(NVDB_ERR_IO, "WAL write failed: %s", strerror(errno));
        free(buf);
        pthread_mutex_unlock(&wal->lock);
        return NVDB_ERR_IO;
    }

    wal->next_lsn++;
    wal->bytes_written += total_len;

    free(buf);
    pthread_mutex_unlock(&wal->lock);
    return NVDB_OK;
}

/* ── Flush to disk ────────────────────────────────────────────── */

int wal_sync(NVDBWAL *wal) {
    pthread_mutex_lock(&wal->lock);
    if (fsync(wal->fd) < 0) {
        nvdb_set_error(NVDB_ERR_IO, "WAL fsync failed: %s", strerror(errno));
        pthread_mutex_unlock(&wal->lock);
        return NVDB_ERR_IO;
    }
    pthread_mutex_unlock(&wal->lock);
    return NVDB_OK;
}

/* ── Truncate (after checkpoint) ──────────────────────────────── */

int wal_truncate(NVDBWAL *wal) {
    pthread_mutex_lock(&wal->lock);

    if (ftruncate(wal->fd, 0) < 0) {
        nvdb_set_error(NVDB_ERR_IO, "WAL truncate failed: %s", strerror(errno));
        pthread_mutex_unlock(&wal->lock);
        return NVDB_ERR_IO;
    }

    wal->next_lsn = 1;
    wal->bytes_written = 0;
    pthread_mutex_unlock(&wal->lock);

    nvdb_log_info("WAL truncated");
    return NVDB_OK;
}

/* ── Recovery ─────────────────────────────────────────────────── */

/*
 * Callback for each recovered WAL record.
 * Return 0 to continue, non-zero to stop.
 */
typedef int (*wal_replay_cb)(nvdb_walop_t op, txnid_t txn_id,
                              const void *key, uint16_t keylen,
                              const void *value, uint32_t vallen,
                              void *arg);

int wal_recover(NVDBWAL *wal, NVDBBTree *tree) {
    pthread_mutex_lock(&wal->lock);

    /* Read entire WAL into memory */
    off_t sz = lseek(wal->fd, 0, SEEK_END);
    if (sz == 0) {
        pthread_mutex_unlock(&wal->lock);
        nvdb_log_info("WAL empty, no recovery needed");
        return NVDB_OK;
    }

    uint8_t *data = nvdb_malloc((size_t)sz);
    if (pread(wal->fd, data, (size_t)sz, 0) != sz) {
        nvdb_set_error(NVDB_ERR_IO, "WAL read failed: %s", strerror(errno));
        free(data);
        pthread_mutex_unlock(&wal->lock);
        return NVDB_ERR_IO;
    }

    /* Track committed transactions */
    int committed[1024] = {0};  /* simple bitmap, capped at 1024 txns */
    int commit_count = 0;

    /* First pass: find all committed transactions */
    off_t pos = 0;
    while ((size_t)pos + WAL_HDR_FIXED <= (size_t)sz) {
        uint32_t total_len, crc_stored, crc_computed;
        uint8_t  op;

        memcpy(&total_len,  data + pos + WAL_LEN_OFF, 4);
        memcpy(&crc_stored, data + pos + WAL_CRC_OFF, 4);

        if (total_len < WAL_HDR_FIXED || (size_t)pos + total_len > (size_t)sz)
            break;

        crc_computed = crc32c_compute(data + pos + 4, total_len - 4);
        if (crc_stored != crc_computed) {
            nvdb_log_warn("WAL: CRC mismatch at offset %ld, truncating", pos);
            break;
        }

        op = data[pos + WAL_OP_OFF];
        if (op == NVDB_WAL_COMMIT) {
            txnid_t tid;
            memcpy(&tid, data + pos + WAL_TXN_OFF, 4);
            if (tid < 1024) {
                committed[tid] = 1;
                commit_count++;
            }
        }

        pos += total_len;
    }

    if (commit_count == 0) {
        nvdb_log_info("WAL: no committed transactions to replay");
        free(data);
        pthread_mutex_unlock(&wal->lock);
        return wal_truncate(wal);
    }

    /* Second pass: replay committed transactions */
    pos = 0;
    int replayed = 0;
    while ((size_t)pos + WAL_HDR_FIXED <= (size_t)sz) {
        uint32_t total_len, crc_stored, crc_computed, vallen;
        uint16_t keylen;
        txnid_t  txn_id;
        uint8_t  op;

        memcpy(&total_len,  data + pos + WAL_LEN_OFF, 4);
        if (total_len < WAL_HDR_FIXED || (size_t)pos + total_len > (size_t)sz)
            break;

        memcpy(&crc_stored, data + pos + WAL_CRC_OFF, 4);
        crc_computed = crc32c_compute(data + pos + 4, total_len - 4);
        if (crc_stored != crc_computed) break;

        memcpy(&txn_id, data + pos + WAL_TXN_OFF, 4);
        op    = data[pos + WAL_OP_OFF];
        memcpy(&keylen, data + pos + WAL_KLEN_OFF, 2);
        memcpy(&vallen, data + pos + WAL_VLEN_OFF, 4);

        if (txn_id < 1024 && committed[txn_id]) {
            const uint8_t *key   = data + pos + WAL_HDR_FIXED;
            const uint8_t *value = key + keylen;

            switch (op) {
            case NVDB_WAL_INSERT:
                btree_insert(tree, key, keylen, value, (uint16_t)vallen);
                replayed++;
                break;
            case NVDB_WAL_DELETE:
                btree_delete(tree, key, keylen);
                replayed++;
                break;
            default:
                break;
            }
        }

        pos += total_len;
    }

    free(data);
    nvdb_log_info("WAL recovery: replayed %d records from %d txns",
                  replayed, commit_count);

    pthread_mutex_unlock(&wal->lock);
    return wal_truncate(wal);
}
