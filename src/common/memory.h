/*
 * memory.h — Internal memory management interface
 */
#ifndef NOVDB_MEMORY_H
#define NOVDB_MEMORY_H

#include <stddef.h>
#include "novadb/types.h"

/* ── General-purpose allocators (abort on OOM) ────────────────── */
void  *nvdb_malloc(size_t size);
void  *nvdb_calloc(size_t nmemb, size_t size);
void  *nvdb_realloc(void *ptr, size_t size);
char  *nvdb_strdup(const char *s);

/* ── Arena: bump-pointer, freed all at once ───────────────────── */
NVDBArena *nvdb_arena_create(void);
void      *nvdb_arena_alloc(NVDBArena *arena, size_t size);
char      *nvdb_arena_strdup(NVDBArena *arena, const char *s);
void       nvdb_arena_reset(NVDBArena *arena);
void       nvdb_arena_destroy(NVDBArena *arena);

/* ── Slab: fixed-size object cache ────────────────────────────── */
NVDBSlab *nvdb_slab_create(size_t obj_size);
void     *nvdb_slab_alloc(NVDBSlab *slab);
void      nvdb_slab_free(NVDBSlab *slab, void *ptr);
void      nvdb_slab_destroy(NVDBSlab *slab);

#endif /* NOVDB_MEMORY_H */
