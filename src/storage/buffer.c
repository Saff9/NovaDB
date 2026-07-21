/*
 * buffer.c — Buffer pool with clock-sweep (second-chance) eviction
 *
 * The buffer pool is the central cache for all page I/O.  It sits
 * between the B+Tree and the disk, serving reads from memory and
 * deferring writes.
 *
 * Eviction policy: clock sweep (a.k.a. second-chance FIFO).
 * Each cached page has a reference bit.  The clock hand sweeps
 * through the pool; if a page's ref bit is set, it's cleared and
 * the hand moves on.  If it's clear, the page is evicted (flushed
 * to disk first if dirty).
 *
 * This is simple, fast, and performs well for database workloads
 * which exhibit temporal and spatial locality.
 */

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#include "buffer.h"
#include "page.h"
#include "memory.h"
#include "logging.h"

#define NVDB_BUFFER_POOL_SIZE 4096  /* pages — ~32 MB at 8KB/page */

struct NVDBBufferPool {
    int               fd;             /* database file descriptor      */
    NVDBSlottedPage **pages;          /* array of pin_count-sized ptrs */
    uint32_t          capacity;
    uint32_t          npages;         /* currently loaded pages        */

    /* Clock sweep state */
    uint8_t          *ref_bits;       /* one bit per slot              */
    uint32_t          clock_hand;

    /* Page index: pgno -> slot in pages[] (-1 if not cached) */
    /* Simple for now: linear scan. TODO: hash table. */
    pgno_t           *page_map;       /* slot -> pgno                  */

    pthread_mutex_t   lock;
    uint64_t          hits;
    uint64_t          misses;
};

/* Forward */
static int  bp_evict_one(NVDBBufferPool *bp);
static int  bp_read_page(NVDBBufferPool *bp, pgno_t pageno,
                          NVDBSlottedPage *page);

NVDBBufferPool *bp_create(const char *db_path) {
    NVDBBufferPool *bp = nvdb_calloc(1, sizeof(*bp));

    bp->fd = open(db_path, O_RDWR | O_CREAT, 0640);
    if (bp->fd < 0) {
        nvdb_set_error(NVDB_ERR_IO, "cannot open %s: %s",
                       db_path, strerror(errno));
        free(bp);
        return NULL;
    }

    bp->capacity  = NVDB_BUFFER_POOL_SIZE;
    bp->pages     = nvdb_calloc(bp->capacity, sizeof(NVDBSlottedPage *));
    bp->ref_bits  = nvdb_calloc(bp->capacity, sizeof(uint8_t));
    bp->page_map  = nvdb_calloc(bp->capacity, sizeof(pgno_t));

    /* Mark all page_map slots as invalid */
    for (uint32_t i = 0; i < bp->capacity; i++) {
        bp->page_map[i] = PGNO_INVALID;
    }

    pthread_mutex_init(&bp->lock, NULL);

    nvdb_log_info("buffer pool created: %u pages, %zu MB",
                  bp->capacity,
                  (size_t)bp->capacity * NVDB_PAGE_SIZE / (1024 * 1024));
    return bp;
}

void bp_destroy(NVDBBufferPool *bp) {
    if (!bp) return;

    pthread_mutex_lock(&bp->lock);

    /* Flush all dirty pages */
    for (uint32_t i = 0; i < bp->npages; i++) {
        if (bp->pages[i] && bp->pages[i]->dirty) {
            ssize_t n = pwrite(bp->fd, bp->pages[i]->data,
                               NVDB_PAGE_SIZE,
                               (off_t)bp->pages[i]->pageno * NVDB_PAGE_SIZE);
            if (n < 0) {
                nvdb_log_error("flush page %" PRIu64 " failed: %s",
                               bp->pages[i]->pageno, strerror(errno));
            }
        }
        if (bp->pages[i]) {
            free(bp->pages[i]);
        }
    }

    pthread_mutex_unlock(&bp->lock);
    pthread_mutex_destroy(&bp->lock);

    if (bp->fd >= 0) close(bp->fd);
    free(bp->pages);
    free(bp->ref_bits);
    free(bp->page_map);
    free(bp);

    nvdb_log_info("buffer pool destroyed: %" PRIu64 " hits, %" PRIu64 " misses",
                  bp->hits, bp->misses);
}

/* Find slot for a page number; returns -1 if not in cache */
static int32_t bp_find_slot(NVDBBufferPool *bp, pgno_t pageno) {
    for (uint32_t i = 0; i < bp->npages; i++) {
        if (bp->page_map[i] == pageno && bp->pages[i] != NULL)
            return (int32_t)i;
    }
    return -1;
}

NVDBSlottedPage *bp_fetch(NVDBBufferPool *bp, pgno_t pageno) {
    pthread_mutex_lock(&bp->lock);

    /* Check cache */
    int32_t slot = bp_find_slot(bp, pageno);
    if (slot >= 0) {
        bp->ref_bits[slot] = 1;
        bp->hits++;
        NVDBSlottedPage *p = bp->pages[slot];
        p->pin_count++;
        pthread_mutex_unlock(&bp->lock);
        return p;
    }

    bp->misses++;

    /* Make room if full */
    if (bp->npages >= bp->capacity) {
        if (bp_evict_one(bp) != NVDB_OK) {
            pthread_mutex_unlock(&bp->lock);
            return NULL;
        }
    }

    /* Allocate new page and read from disk */
    NVDBSlottedPage *page = nvdb_malloc(sizeof(NVDBSlottedPage));
    int rc = bp_read_page(bp, pageno, page);
    if (rc != NVDB_OK) {
        free(page);
        pthread_mutex_unlock(&bp->lock);
        return NULL;
    }

    slot = (int32_t)bp->npages;
    bp->pages[slot]    = page;
    bp->page_map[slot] = pageno;
    bp->ref_bits[slot] = 1;
    bp->npages++;
    page->pin_count = 1;

    pthread_mutex_unlock(&bp->lock);
    return page;
}

void bp_unpin(NVDBBufferPool *bp, NVDBSlottedPage *page) {
    if (!page) return;
    pthread_mutex_lock(&bp->lock);
    if (page->pin_count > 0) page->pin_count--;
    pthread_mutex_unlock(&bp->lock);
}

void bp_mark_dirty(NVDBBufferPool *bp, NVDBSlottedPage *page) {
    (void)bp;
    page->dirty = true;
}

int bp_flush(NVDBBufferPool *bp, NVDBSlottedPage *page) {
    if (!page->dirty) return NVDB_OK;

    ssize_t n = pwrite(bp->fd, page->data, NVDB_PAGE_SIZE,
                       (off_t)page->pageno * NVDB_PAGE_SIZE);
    if (n != NVDB_PAGE_SIZE) {
        nvdb_set_error(NVDB_ERR_IO, "pwrite page %" PRIu64 " failed: %s",
                       page->pageno, strerror(errno));
        return NVDB_ERR_IO;
    }

    /* Ensure durability — fsync the file */
    if (fsync(bp->fd) < 0) {
        nvdb_log_warn("fsync failed: %s", strerror(errno));
    }

    page->dirty = false;
    return NVDB_OK;
}

int bp_flush_all(NVDBBufferPool *bp) {
    pthread_mutex_lock(&bp->lock);
    int rc = NVDB_OK;
    for (uint32_t i = 0; i < bp->npages; i++) {
        if (bp->pages[i] && bp->pages[i]->dirty) {
            int r = bp_flush(bp, bp->pages[i]);
            if (r != NVDB_OK) rc = r;
        }
    }
    pthread_mutex_unlock(&bp->lock);
    return rc;
}

void bp_stats(const NVDBBufferPool *bp, uint64_t *hits, uint64_t *misses) {
    *hits   = bp->hits;
    *misses = bp->misses;
}

/* ── Clock sweep eviction ─────────────────────────────────────── */

static int bp_evict_one(NVDBBufferPool *bp) {
    uint32_t start = bp->clock_hand;

    for (uint32_t i = 0; i < bp->capacity * 2; i++) {
        uint32_t idx = (start + i) % bp->capacity;

        if (idx >= bp->npages) continue;
        if (!bp->pages[idx]) continue;
        if (bp->pages[idx]->pin_count > 0) continue;

        if (bp->ref_bits[idx]) {
            bp->ref_bits[idx] = 0;
            continue;
        }

        /* Evict this page */
        NVDBSlottedPage *victim = bp->pages[idx];

        if (victim->dirty) {
            ssize_t n = pwrite(bp->fd, victim->data, NVDB_PAGE_SIZE,
                               (off_t)victim->pageno * NVDB_PAGE_SIZE);
            if (n != NVDB_PAGE_SIZE) {
                nvdb_log_error("evict flush page %" PRIu64 " failed: %s",
                               victim->pageno, strerror(errno));
            }
        }

        free(victim);

        /* Compact: move last page into this slot */
        uint32_t last = bp->npages - 1;
        if (idx != last) {
            bp->pages[idx]    = bp->pages[last];
            bp->page_map[idx] = bp->page_map[last];
            bp->ref_bits[idx] = bp->ref_bits[last];
        }
        bp->pages[last]    = NULL;
        bp->page_map[last] = PGNO_INVALID;
        bp->npages--;

        bp->clock_hand = (idx + 1) % bp->capacity;
        return NVDB_OK;
    }

    nvdb_set_error(NVDB_ERR_FULL, "buffer pool full, all pages pinned");
    return NVDB_ERR_FULL;
}

/* ── Read page from disk (or create blank) ────────────────────── */

static int bp_read_page(NVDBBufferPool *bp, pgno_t pageno,
                         NVDBSlottedPage *page) {
    memset(page, 0, sizeof(*page));
    page->pageno = pageno;

    off_t offset = (off_t)pageno * NVDB_PAGE_SIZE;

    /* Check if the file extends far enough */
    off_t end = lseek(bp->fd, 0, SEEK_END);
    if (end < 0) {
        nvdb_set_error(NVDB_ERR_IO, "lseek failed: %s", strerror(errno));
        return NVDB_ERR_IO;
    }

    if (offset >= end) {
        /* Page doesn't exist yet — caller must initialise */
        memset(page->data, 0, NVDB_PAGE_SIZE);
        return NVDB_OK;
    }

    ssize_t n = pread(bp->fd, page->data, NVDB_PAGE_SIZE, offset);
    if (n < 0) {
        nvdb_set_error(NVDB_ERR_IO, "pread page %" PRIu64 ": %s",
                       pageno, strerror(errno));
        return NVDB_ERR_IO;
    }
    if (n < NVDB_PAGE_SIZE) {
        /* Partial read — zero the rest */
        memset(page->data + n, 0, (size_t)(NVDB_PAGE_SIZE - n));
    }

    /* Validate magic */
    NVDBPageHeader hdr;
    page_decode_header(page->data, &hdr);
    if (hdr.magic != NVDB_PAGE_MAGIC && hdr.magic != 0) {
        nvdb_log_warn("page %" PRIu64 " has bad magic 0x%08X",
                      pageno, hdr.magic);
    }

    return NVDB_OK;
}

int bp_fd(const NVDBBufferPool *bp) {
    return bp->fd;
}
