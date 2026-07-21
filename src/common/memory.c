/*
 * memory.c — Arena and slab allocators
 *
 * Arena: bump-pointer allocator. Each arena is a linked list of
 * 64KB blocks. Allocations are served from the current block's
 * free region. No individual frees — the entire arena is released
 * at once when the query completes. This avoids the vast majority
 * of use-after-free, double-free, and leak bugs.
 *
 * Slab: per-size-class object cache. Each slab pre-allocates
 * chunks of 64 objects and maintains a free list. Allocations
 * and frees are O(1). Used for pages, cursors, transaction
 * descriptors, and other frequently-created/destroyed objects.
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "memory.h"
#include "novadb/error.h"

/* ── OOM handler ──────────────────────────────────────────────── */

static NVDB_NORETURN void oom_panic(size_t requested) {
    fprintf(stderr, "NovaDB: out of memory allocating %zu bytes\n",
            requested);
    abort();
}

void *nvdb_malloc(size_t size) {
    void *p = malloc(size);
    if (NVDB_UNLIKELY(!p && size > 0)) oom_panic(size);
    return p;
}

void *nvdb_calloc(size_t nmemb, size_t size) {
    void *p = calloc(nmemb, size);
    if (NVDB_UNLIKELY(!p && nmemb > 0 && size > 0))
        oom_panic(nmemb * size);
    return p;
}

void *nvdb_realloc(void *ptr, size_t size) {
    void *p = realloc(ptr, size);
    if (NVDB_UNLIKELY(!p && size > 0)) oom_panic(size);
    return p;
}

char *nvdb_strdup(const char *s) {
    if (!s) return NULL;
    size_t len = strlen(s) + 1;
    char *d = nvdb_malloc(len);
    memcpy(d, s, len);
    return d;
}

/* ── Arena allocator ──────────────────────────────────────────── */

#define ARENA_BLOCK_SZ (64 * 1024)

typedef struct ArenaBlock ArenaBlock;
struct ArenaBlock {
    ArenaBlock *next;
    char       *base;     /* start of data region */
    size_t      used;
    size_t      capacity;
    /* data follows the struct in the same allocation */
};

struct NVDBArena {
    ArenaBlock *head;
    ArenaBlock *current;
    size_t      total_bytes;
};

NVDBArena *nvdb_arena_create(void) {
    NVDBArena *arena = nvdb_malloc(sizeof(*arena));

    ArenaBlock *b   = nvdb_malloc(sizeof(ArenaBlock));
    b->base         = nvdb_malloc(ARENA_BLOCK_SZ);
    b->next         = NULL;
    b->used         = 0;
    b->capacity     = ARENA_BLOCK_SZ;

    arena->head        = b;
    arena->current     = b;
    arena->total_bytes = ARENA_BLOCK_SZ;
    return arena;
}

void *nvdb_arena_alloc(NVDBArena *arena, size_t size) {
    size = (size + 7) & ~(size_t)7;  /* 8-byte alignment */

    /* Oversized — allocate standalone */
    if (NVDB_UNLIKELY(size > ARENA_BLOCK_SZ / 2)) {
        ArenaBlock *b = nvdb_malloc(sizeof(ArenaBlock));
        b->base     = nvdb_malloc(size);
        b->used     = size;
        b->capacity = size;
        b->next     = arena->current->next;
        arena->current->next = b;
        arena->total_bytes += size;
        return b->base;
    }

    ArenaBlock *b = arena->current;
    if (NVDB_UNLIKELY(b->used + size > b->capacity)) {
        ArenaBlock *nb = nvdb_malloc(sizeof(ArenaBlock));
        nb->base     = nvdb_malloc(ARENA_BLOCK_SZ);
        nb->next     = NULL;
        nb->used     = 0;
        nb->capacity = ARENA_BLOCK_SZ;
        b->next      = nb;
        arena->current = nb;
        arena->total_bytes += ARENA_BLOCK_SZ;
        b = nb;
    }

    void *ptr = b->base + b->used;
    b->used += size;
    return ptr;
}

char *nvdb_arena_strdup(NVDBArena *arena, const char *s) {
    if (!s) return NULL;
    size_t len = strlen(s) + 1;
    char *d = nvdb_arena_alloc(arena, len);
    memcpy(d, s, len);
    return d;
}

void nvdb_arena_reset(NVDBArena *arena) {
    ArenaBlock *b = arena->head;
    while (b) {
        b->used = 0;
        b = b->next;
    }
    arena->current = arena->head;
}

void nvdb_arena_destroy(NVDBArena *arena) {
    if (!arena) return;
    ArenaBlock *b = arena->head;
    while (b) {
        ArenaBlock *next = b->next;
        free(b->base);
        free(b);
        b = next;
    }
    free(arena);
}

/* ── Slab allocator ──────────────────────────────────────────── */

#define SLAB_OBJS_PER_CHUNK 64

typedef struct SlabChunk SlabChunk;
struct SlabChunk {
    SlabChunk *next;
    char      *base;
};

struct NVDBSlab {
    size_t      obj_size;
    SlabChunk  *chunk_head;
    void       *free_list;
    uint64_t    allocated;
    uint64_t    in_use;
};

NVDBSlab *nvdb_slab_create(size_t obj_size) {
    if (obj_size < sizeof(void *)) obj_size = sizeof(void *);
    obj_size = (obj_size + 7) & ~(size_t)7;

    NVDBSlab *slab = nvdb_calloc(1, sizeof(*slab));
    slab->obj_size = obj_size;
    return slab;
}

static void slab_refill(NVDBSlab *slab) {
    size_t chunk_bytes = SLAB_OBJS_PER_CHUNK * slab->obj_size;

    SlabChunk *chunk = nvdb_malloc(sizeof(SlabChunk));
    chunk->base = nvdb_malloc(chunk_bytes);
    chunk->next = slab->chunk_head;
    slab->chunk_head = chunk;
    slab->allocated  += SLAB_OBJS_PER_CHUNK;

    /* Thread free list through each object */
    char *p = chunk->base;
    for (int i = 0; i < SLAB_OBJS_PER_CHUNK; i++) {
        void *obj = p + (size_t)i * slab->obj_size;
        *(void **)obj = slab->free_list;
        slab->free_list = obj;
    }
}

void *nvdb_slab_alloc(NVDBSlab *slab) {
    if (NVDB_UNLIKELY(!slab->free_list))
        slab_refill(slab);

    void *obj = slab->free_list;
    slab->free_list = *(void **)obj;
    slab->in_use++;
    memset(obj, 0, slab->obj_size);
    return obj;
}

void nvdb_slab_free(NVDBSlab *slab, void *ptr) {
    if (!ptr) return;
    *(void **)ptr = slab->free_list;
    slab->free_list = ptr;
    slab->in_use--;
}

void nvdb_slab_destroy(NVDBSlab *slab) {
    if (!slab) return;
    SlabChunk *c = slab->chunk_head;
    while (c) {
        SlabChunk *next = c->next;
        free(c->base);
        free(c);
        c = next;
    }
    free(slab);
}

/* ── Error context ────────────────────────────────────────────── */

NVDB_THREAD_LOCAL NVDBError nvdb_last_error;

NVDB_NORETURN void nvdb_panic(const char *file, int line,
                               const char *func, const char *msg) {
    fprintf(stderr, "NovaDB PANIC at %s:%d in %s(): %s\n",
            file, line, func, msg);
    fflush(stderr);
    abort();
}

const char *nvdb_strerror(nvdb_errcode_t code) {
    switch (code) {
    case NVDB_OK:               return "success";
    case NVDB_ERR_IO:           return "I/O error";
    case NVDB_ERR_NOMEM:        return "out of memory";
    case NVDB_ERR_CORRUPT:      return "data corruption detected";
    case NVDB_ERR_NOT_FOUND:    return "key not found";
    case NVDB_ERR_DUPLICATE:    return "duplicate key";
    case NVDB_ERR_FULL:         return "disk full";
    case NVDB_ERR_PROTOCOL:     return "protocol error";
    case NVDB_ERR_PARSE:        return "SQL parse error";
    case NVDB_ERR_EXEC:         return "execution error";
    case NVDB_ERR_TXN_ABORTED:  return "transaction aborted";
    case NVDB_ERR_TXN_CONFLICT: return "write conflict, retry";
    case NVDB_ERR_TXN_EXPIRED:  return "transaction expired";
    case NVDB_ERR_CONN_LIMIT:   return "connection limit reached";
    case NVDB_ERR_TIMEOUT:      return "operation timed out";
    case NVDB_ERR_NOT_IMPL:     return "not implemented";
    case NVDB_ERR_INTERNAL:     return "internal error";
    default:                    return "unknown error";
    }
}
