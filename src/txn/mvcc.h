/*
 * mvcc.h — Multi-Version Concurrency Control (MVCC) interface
 *
 * Declares the MVCC visibility predicate used by the executor and
 * transaction manager to decide which row versions are visible to
 * a given snapshot transaction.
 */
#ifndef NOVDB_MVCC_H
#define NOVDB_MVCC_H

#include "novadb/types.h"

/*
 * mvcc_is_visible() — returns non-zero if a row written by
 * writer_tid is visible to a transaction that started at
 * snapshot_tid.
 *
 * writer_committed should be 1 if the writer has committed, 0
 * if it is still in-progress or aborted.
 */
int mvcc_is_visible(txnid_t snapshot_tid, txnid_t writer_tid,
                    int writer_committed);

#endif /* NOVDB_MVCC_H */
