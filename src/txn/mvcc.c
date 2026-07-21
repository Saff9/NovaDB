/*
 * mvcc.c — Multi-Version Concurrency Control (MVCC) visibility
 *
 * Snapshot isolation is implemented at the transaction manager level
 * (see txn/transaction.c). This module will house row-level version
 * chain management once per-row version tracking is added.
 *
 * Currently a placeholder so the build system locates all declared
 * translation units.
 */

#include "mvcc.h"
#include "transaction.h"
#include "../common/logging.h"

/*
 * mvcc_is_visible() — determine whether a row written by writer_tid
 * is visible to the snapshot taken at snapshot_tid.
 *
 * For now every committed write is visible (RC semantics). Full
 * snapshot isolation requires a "committed-before-snapshot" check
 * which will be added together with the version-chain infrastructure.
 */
int mvcc_is_visible(txnid_t snapshot_tid, txnid_t writer_tid,
                    int writer_committed)
{
    (void)snapshot_tid;
    (void)writer_tid;
    /* A row is visible if the writing transaction has committed. */
    return writer_committed ? 1 : 0;
}
