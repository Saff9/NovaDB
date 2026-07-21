/*
 * nvdb.h — Public API for NovaDB embedded database engine
 *
 * Usage:
 *   1. nvdb_open()         — open or create a database
 *   2. nvdb_begin()        — start a transaction
 *   3. nvdb_exec()         — run SQL within the transaction
 *   4. nvdb_commit() / nvdb_rollback() — finish
 *   5. nvdb_close()        — clean shutdown
 *
 * All functions are thread-safe. A handle may be used concurrently
 * from multiple threads; the engine manages internal locking.
 */
#ifndef NOVDB_H
#define NOVDB_H

#include "types.h"
#include "error.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Lifecycle ────────────────────────────────────────────────── */

/*
 * Open a database at `path`. Creates the directory and all required
 * files if they do not exist. Returns NULL on failure — check
 * nvdb_last_error for details.
 */
NVDBEngine *nvdb_open(const char *path);

/*
 * Close the database, flushing all dirty pages and the WAL.
 * Returns NVDB_OK on success.
 */
nvdb_errcode_t nvdb_close(NVDBEngine *engine);

/* ── Transactions ─────────────────────────────────────────────── */

/*
 * Begin a new transaction with snapshot isolation.
 * The returned handle is valid until committed or rolled back.
 */
NVDBTransaction *nvdb_begin(NVDBEngine *engine);

/*
 * Commit an active transaction. All writes become durable and
 * visible to subsequent transactions.
 */
nvdb_errcode_t nvdb_commit(NVDBTransaction *txn);

/*
 * Roll back an active transaction. All writes are discarded.
 */
nvdb_errcode_t nvdb_rollback(NVDBTransaction *txn);

/* ── SQL execution ────────────────────────────────────────────── */

/*
 * Execute a SQL statement within the given transaction.
 * For auto-commit behaviour, pass the engine handle directly
 * (a short-lived transaction will be created internally).
 *
 * Returns a ResultSet that the caller must free with nvdb_result_free().
 * Returns NULL on error — check nvdb_last_error.
 */
NVDBResultSet *nvdb_exec(NVDBEngine *engine, const char *sql);

/*
 * Execute SQL within an explicit transaction.
 */
NVDBResultSet *nvdb_exec_txn(NVDBTransaction *txn, const char *sql);

/* ── ResultSet inspection ─────────────────────────────────────── */

/*
 * Number of columns in the result.
 */
uint32_t nvdb_result_ncols(const NVDBResultSet *rs);

/*
 * Number of rows in the result.
 */
uint64_t nvdb_result_nrows(const NVDBResultSet *rs);

/*
 * Column name at index `col` (0-based). Returns NULL if out of range.
 */
const char *nvdb_result_colname(const NVDBResultSet *rs, uint32_t col);

/*
 * Value at (row, col). Returns a pointer valid until the ResultSet
 * is freed. Returns NULL for out-of-range or NULL values.
 */
const NVDBValue *nvdb_result_value(const NVDBResultSet *rs,
                                    uint64_t row, uint32_t col);

/*
 * Free a ResultSet. Safe to call with NULL.
 */
void nvdb_result_free(NVDBResultSet *rs);

/* ── Direct key-value access ──────────────────────────────────── */

/*
 * Get a value by key (bypasses SQL). Returns NVDB_OK if found,
 * NVDB_ERR_NOT_FOUND otherwise. The caller must free *value
 * with free().
 */
nvdb_errcode_t nvdb_kv_get(NVDBEngine *engine,
                            const char *key, char **value, size_t *vallen);

/*
 * Put a key-value pair (auto-commit).
 */
nvdb_errcode_t nvdb_kv_put(NVDBEngine *engine,
                            const char *key, const char *value);

/*
 * Delete a key.
 */
nvdb_errcode_t nvdb_kv_del(NVDBEngine *engine, const char *key);

/* ── Statistics ───────────────────────────────────────────────── */

typedef struct {
    uint64_t pages_read;
    uint64_t pages_written;
    uint64_t wal_bytes_written;
    uint64_t transactions_committed;
    uint64_t transactions_rolled_back;
    uint64_t rows_inserted;
    uint64_t rows_deleted;
    uint64_t rows_updated;
    uint64_t rows_read;
    uint64_t buffer_pool_hits;
    uint64_t buffer_pool_misses;
    uint64_t active_connections;
    double   uptime_seconds;
} NVDBStats;

const NVDBStats *nvdb_stats(NVDBEngine *engine);

/* ── Version ──────────────────────────────────────────────────── */
const char *nvdb_version(void);

#ifdef __cplusplus
}
#endif

#endif /* NOVDB_H */
