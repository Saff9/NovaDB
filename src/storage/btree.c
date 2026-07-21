/*
 * btree.c — B+Tree with copy-on-write
 *
 * All data in leaf nodes; internal nodes store separator keys
 * and 8-byte child page numbers. Leaf nodes form a singly-linked
 * list for efficient range scans.
 *
 * Meta page (page 0) stores: root pgno, next available pgno,
 * total key count, and a secondary magic number for format detection.
 */

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <inttypes.h>
#include "btree.h"
#include "page.h"
#include "memory.h"
#include "logging.h"
#include "novadb/error.h"

/* ── Meta page (page 0) layout ────────────────────────────────── */
#define META_ROOT      40
#define META_NEXT_PG   48
#define META_NKEYS     56
#define META_MAGIC2    64
#define META_MAGIC2_VAL 0x4244564E

struct NVDBBTree {
    NVDBBufferPool *bp;
    pgno_t          root;
    pgno_t          next_page;
    uint64_t        key_count;
};

/* ── Internal helpers ─────────────────────────────────────────── */

void read_slot_internal(const NVDBSlottedPage *page, uint16_t idx,
                         NVDBSlotEntry *e) {
    page_read_slot_entry(page->data, idx, e);
}

static int meta_load(NVDBBTree *tree) {
    NVDBSlottedPage *m = bp_fetch(tree->bp, 0);
    if (!m) return NVDB_ERR_IO;

    uint32_t m2;
    memcpy(&m2, m->data + META_MAGIC2, 4);

    NVDBPageHeader hdr;
    page_decode_header(m->data, &hdr);

    int is_fresh = (hdr.magic != NVDB_PAGE_MAGIC || m2 != META_MAGIC2_VAL);

    if (is_fresh) {
        /* Initialise a new database */
        tree->root      = 1;
        tree->next_page = 2;
        tree->key_count = 0;

        /* Create root as empty leaf */
        NVDBSlottedPage *root_pg = bp_fetch(tree->bp, 1);
        if (!root_pg) { bp_unpin(tree->bp, m); return NVDB_ERR_IO; }
        page_init(root_pg, 1, NVDB_PAGE_BTREE_LEAF);
        bp_mark_dirty(tree->bp, root_pg);
        bp_unpin(tree->bp, root_pg);

        /* Write meta page */
        hdr.magic     = NVDB_PAGE_MAGIC;
        hdr.page_type = NVDB_PAGE_META;
        hdr.self      = 0;
        page_encode_header(m->data, &hdr);
    } else {
        memcpy(&tree->root,      m->data + META_ROOT,    8);
        memcpy(&tree->next_page, m->data + META_NEXT_PG, 8);
        memcpy(&tree->key_count, m->data + META_NKEYS,   8);
    }

    bp_unpin(tree->bp, m);
    return NVDB_OK;
}

static int meta_sync(NVDBBTree *tree) {
    NVDBSlottedPage *m = bp_fetch(tree->bp, 0);
    if (!m) return NVDB_ERR_IO;

    memcpy(m->data + META_ROOT,    &tree->root,      8);
    memcpy(m->data + META_NEXT_PG, &tree->next_page, 8);
    memcpy(m->data + META_NKEYS,   &tree->key_count, 8);

    uint32_t m2 = META_MAGIC2_VAL;
    memcpy(m->data + META_MAGIC2, &m2, 4);

    NVDBPageHeader hdr = {
        .magic = NVDB_PAGE_MAGIC, .page_type = NVDB_PAGE_META, .self = 0,
    };
    page_encode_header(m->data, &hdr);
    bp_mark_dirty(tree->bp, m);
    bp_unpin(tree->bp, m);
    return NVDB_OK;
}

/*
 * Find the child page for a given key in an internal node.
 * Internal node slot format: [separator_key][child_pgno:8]
 * The child pointer is the one whose separator key is >= search key.
 */
static int internal_find_child(NVDBSlottedPage *page,
                                const void *key, uint16_t keylen,
                                pgno_t *child_out) {
    NVDBPageHeader hdr;
    page_decode_header(page->data, &hdr);

    pgno_t child = PGNO_INVALID;

    for (uint16_t i = 0; i < hdr.nslots; i++) {
        NVDBSlotEntry e;
        page_read_slot_entry(page->data, i, &e);

        uint16_t cmp_len = (keylen < e.key_length) ? keylen : e.key_length;
        int cmp = memcmp(key, page->data + e.offset, cmp_len);

        if (cmp < 0 || (cmp == 0 && keylen < e.key_length)) {
            memcpy(&child, page->data + e.offset + e.key_length, 8);
            break;
        }
        /* Last resort: update child to this slot's pointer */
        memcpy(&child, page->data + e.offset + e.key_length, 8);
    }

    if (child == PGNO_INVALID) return NVDB_ERR_CORRUPT;
    *child_out = child;
    return NVDB_OK;
}

/*
 * Navigate from root to the leaf that would contain `key`.
 * Returns the leaf page number in *leaf_out.
 */
static int navigate_to_leaf(NVDBBTree *tree, pgno_t from,
                             const void *key, uint16_t keylen,
                             pgno_t *leaf_out) {
    NVDBSlottedPage *pg = bp_fetch(tree->bp, from);
    if (!pg) return NVDB_ERR_IO;

    NVDBPageHeader hdr;
    page_decode_header(pg->data, &hdr);

    int rc;
    if (hdr.page_type == NVDB_PAGE_BTREE_LEAF) {
        *leaf_out = from;
        rc = NVDB_OK;
    } else if (hdr.page_type == NVDB_PAGE_BTREE_INT) {
        pgno_t child;
        rc = internal_find_child(pg, key, keylen, &child);
        bp_unpin(tree->bp, pg);
        if (rc != NVDB_OK) return rc;
        return navigate_to_leaf(tree, child, key, keylen, leaf_out);
    } else {
        rc = NVDB_ERR_CORRUPT;
    }

    bp_unpin(tree->bp, pg);
    return rc;
}

/*
 * Split a full leaf page. The right half moves to a new page.
 * The first key of the right half is returned as the separator.
 */
static int split_leaf(NVDBBTree *tree, NVDBSlottedPage *left,
                       pgno_t left_pg,
                       NVDBSlottedPage **right_out, pgno_t *right_pg_out,
                       void *sep_key, uint16_t *sep_keylen) {
    NVDBPageHeader left_hdr;
    page_decode_header(left->data, &left_hdr);

    uint16_t mid = left_hdr.nslots / 2;

    /* Create right sibling */
    *right_pg_out = tree->next_page++;
    NVDBSlottedPage *right = bp_fetch(tree->bp, *right_pg_out);
    page_init(right, *right_pg_out, NVDB_PAGE_BTREE_LEAF);

    /* Get separator key from first slot of right half */
    NVDBSlotEntry sep_e;
    page_read_slot_entry(left->data, mid, &sep_e);
    memcpy(sep_key, left->data + sep_e.offset, sep_e.key_length);
    *sep_keylen = sep_e.key_length;

    /* Move slots mid..end to the right page */
    uint16_t orig_count = left_hdr.nslots;
    for (uint16_t i = mid; i < orig_count; i++) {
        NVDBSlotEntry e;
        page_read_slot_entry(left->data, i, &e);

        uint16_t total = e.key_length + e.value_length;
        uint8_t  tmp[NVDB_PAGE_SIZE];
        memcpy(tmp, left->data + e.offset, total);

        page_insert(right, tmp, e.key_length,
                    tmp + e.key_length, e.value_length);
    }

    /* Remove the moved slots from left page (from highest index down) */
    for (uint16_t i = orig_count; i > mid; i--) {
        page_delete_slot(left, (uint16_t)(i - 1));
    }

    /* Update sibling chain */
    NVDBPageHeader new_left_hdr, new_right_hdr;
    page_decode_header(left->data, &new_left_hdr);
    page_decode_header(right->data, &new_right_hdr);
    new_right_hdr.right_sibling = new_left_hdr.right_sibling;
    new_left_hdr.right_sibling  = *right_pg_out;
    page_encode_header(left->data, &new_left_hdr);
    page_encode_header(right->data, &new_right_hdr);

    bp_mark_dirty(tree->bp, left);
    bp_mark_dirty(tree->bp, right);
    *right_out = right;
    return NVDB_OK;
}

/*
 * Create a new root internal node pointing to left and right children.
 */
static int create_new_root(NVDBBTree *tree,
                            pgno_t left_child, pgno_t right_child,
                            const void *sep_key, uint16_t sep_keylen) {
    pgno_t new_root_pg = tree->next_page++;
    NVDBSlottedPage *nr = bp_fetch(tree->bp, new_root_pg);
    if (!nr) return NVDB_ERR_IO;

    page_init(nr, new_root_pg, NVDB_PAGE_BTREE_INT);

    /* Slot 0: empty key -> left child */
    uint8_t zero = 0;
    page_insert(nr, &zero, 1, &left_child, sizeof(pgno_t));

    /* Slot 1: separator key -> right child */
    page_insert(nr, sep_key, sep_keylen, &right_child, sizeof(pgno_t));

    bp_mark_dirty(tree->bp, nr);
    tree->root = new_root_pg;
    bp_unpin(tree->bp, nr);
    return NVDB_OK;
}

/* ── Public interface ─────────────────────────────────────────── */

NVDBBTree *btree_open(NVDBBufferPool *bp) {
    NVDBBTree *tree = nvdb_calloc(1, sizeof(*tree));
    tree->bp = bp;

    if (meta_load(tree) != NVDB_OK) {
        free(tree);
        return NULL;
    }

    nvdb_log_info("btree: root=%" PRIu64 ", next=%" PRIu64 ", keys=%" PRIu64,
                  tree->root, tree->next_page, tree->key_count);
    return tree;
}

void btree_close(NVDBBTree *tree) {
    if (!tree) return;
    meta_sync(tree);
    free(tree);
}

int btree_search(NVDBBTree *tree,
                 const void *key, uint16_t keylen,
                 void *value_out, uint16_t *vallen_out,
                 uint16_t max_vallen) {
    pgno_t leaf_pg;
    int rc = navigate_to_leaf(tree, tree->root, key, keylen, &leaf_pg);
    if (rc != NVDB_OK) return rc;

    NVDBSlottedPage *leaf = bp_fetch(tree->bp, leaf_pg);
    if (!leaf) return NVDB_ERR_IO;

    NVDBSlotEntry e;
    rc = page_find(leaf, key, keylen, &e, NULL);
    if (rc == NVDB_OK && value_out) {
        uint16_t n = (e.value_length < max_vallen) ? e.value_length : max_vallen;
        memcpy(value_out, leaf->data + e.offset + e.key_length, n);
        if (vallen_out) *vallen_out = e.value_length;
    }

    bp_unpin(tree->bp, leaf);
    return rc;
}

int btree_insert(NVDBBTree *tree,
                 const void *key, uint16_t keylen,
                 const void *value, uint16_t vallen) {
    pgno_t leaf_pg;
    int rc = navigate_to_leaf(tree, tree->root, key, keylen, &leaf_pg);
    if (rc != NVDB_OK) return rc;

    NVDBSlottedPage *leaf = bp_fetch(tree->bp, leaf_pg);
    if (!leaf) return NVDB_ERR_IO;

    rc = page_insert(leaf, key, keylen, value, vallen);

    if (rc == NVDB_ERR_DUPLICATE) {
        /* Update in place */
        rc = page_update(leaf, key, keylen, value, vallen);
        if (rc == NVDB_OK) bp_mark_dirty(tree->bp, leaf);

    } else if (rc == NVDB_ERR_FULL) {
        /* Split needed */
        uint8_t  sep_key[NVDB_MAX_KEY_SIZE];
        uint16_t sep_keylen;
        NVDBSlottedPage *right;
        pgno_t   right_pg;

        rc = split_leaf(tree, leaf, leaf_pg, &right, &right_pg,
                        sep_key, &sep_keylen);
        if (rc != NVDB_OK) {
            bp_unpin(tree->bp, leaf);
            return rc;
        }

        /* Insert into correct half */
        int cmp = memcmp(key, sep_key,
                         (keylen < sep_keylen) ? keylen : sep_keylen);
        if (cmp < 0 || (cmp == 0 && keylen <= sep_keylen)) {
            rc = page_insert(leaf, key, keylen, value, vallen);
        } else {
            rc = page_insert(right, key, keylen, value, vallen);
        }

        bp_mark_dirty(tree->bp, leaf);
        bp_mark_dirty(tree->bp, right);

        /* If this was the root, we need a new root */
        if (leaf_pg == tree->root) {
            rc = create_new_root(tree, leaf_pg, right_pg,
                                 sep_key, sep_keylen);
        }

        bp_unpin(tree->bp, right);

    } else if (rc == NVDB_OK) {
        bp_mark_dirty(tree->bp, leaf);
    }

    bp_unpin(tree->bp, leaf);

    if (rc == NVDB_OK) {
        tree->key_count++;
        meta_sync(tree);
    }

    return rc;
}

int btree_delete(NVDBBTree *tree, const void *key, uint16_t keylen) {
    pgno_t leaf_pg;
    int rc = navigate_to_leaf(tree, tree->root, key, keylen, &leaf_pg);
    if (rc != NVDB_OK) return rc;

    NVDBSlottedPage *leaf = bp_fetch(tree->bp, leaf_pg);
    if (!leaf) return NVDB_ERR_IO;

    uint16_t idx;
    rc = page_find(leaf, key, keylen, NULL, &idx);
    if (rc == NVDB_OK) {
        rc = page_delete_slot(leaf, idx);
        if (rc == NVDB_OK) {
            bp_mark_dirty(tree->bp, leaf);
            tree->key_count--;
            meta_sync(tree);
        }
    }

    bp_unpin(tree->bp, leaf);
    return rc;
}

int btree_scan(NVDBBTree *tree,
               const void *start_key, uint16_t start_keylen,
               const void *end_key,   uint16_t end_keylen,
               int (*cb)(const void *k, uint16_t kl,
                         const void *v, uint16_t vl, void *arg),
               void *arg) {
    pgno_t cur_pg;

    if (start_key && start_keylen > 0) {
        int rc = navigate_to_leaf(tree, tree->root,
                                   start_key, start_keylen, &cur_pg);
        if (rc != NVDB_OK) return rc;
    } else {
        /* Walk to leftmost leaf */
        cur_pg = tree->root;
        for (;;) {
            NVDBSlottedPage *p = bp_fetch(tree->bp, cur_pg);
            if (!p) return NVDB_ERR_IO;
            NVDBPageHeader h;
            page_decode_header(p->data, &h);
            if (h.page_type == NVDB_PAGE_BTREE_LEAF) {
                bp_unpin(tree->bp, p);
                break;
            }
            NVDBSlotEntry e;
            page_read_slot_entry(p->data, 0, &e);
            memcpy(&cur_pg, p->data + e.offset + e.key_length, 8);
            bp_unpin(tree->bp, p);
        }
    }

    /* Walk the leaf chain */
    while (cur_pg != PGNO_INVALID) {
        NVDBSlottedPage *leaf = bp_fetch(tree->bp, cur_pg);
        if (!leaf) return NVDB_ERR_IO;

        NVDBPageHeader hdr;
        page_decode_header(leaf->data, &hdr);

        for (uint16_t i = 0; i < hdr.nslots; i++) {
            NVDBSlotEntry e;
            page_read_slot_entry(leaf->data, i, &e);

            const uint8_t *k = leaf->data + e.offset;
            const uint8_t *v = k + e.key_length;

            /* Apply range filters */
            if (start_key && start_keylen > 0) {
                int cmp = memcmp(k, start_key,
                                 (e.key_length < start_keylen)
                                 ? e.key_length : start_keylen);
                if (cmp < 0) continue;
                if (cmp == 0 && e.key_length < start_keylen) continue;
            }
            if (end_key && end_keylen > 0) {
                int cmp = memcmp(k, end_key,
                                 (e.key_length < end_keylen)
                                 ? e.key_length : end_keylen);
                if (cmp > 0) { bp_unpin(tree->bp, leaf); return NVDB_OK; }
                if (cmp == 0 && e.key_length > end_keylen)
                    { bp_unpin(tree->bp, leaf); return NVDB_OK; }
            }

            int cb_rc = cb(k, e.key_length, v, e.value_length, arg);
            if (cb_rc) {
                bp_unpin(tree->bp, leaf);
                return cb_rc;
            }
        }

        pgno_t next = hdr.right_sibling;
        bp_unpin(tree->bp, leaf);
        cur_pg = next;
    }

    return NVDB_OK;
}

/* ── Accessors ────────────────────────────────────────────────── */

pgno_t   btree_root(const NVDBBTree *t)      { return t->root; }
pgno_t   btree_next_page(const NVDBBTree *t)  { return t->next_page; }
uint64_t btree_key_count(const NVDBBTree *t)  { return t->key_count; }

/* ── Key comparison ───────────────────────────────────────────── */

int btree_key_cmp(const void *a, uint16_t alen,
                  const void *b, uint16_t blen) {
    uint16_t n = (alen < blen) ? alen : blen;
    int cmp = memcmp(a, b, n);
    if (cmp != 0) return cmp;
    if (alen < blen) return -1;
    if (alen > blen) return 1;
    return 0;
}
