#pragma once
/*
 * arena.h — Region-based memory allocator.
 *
 * Design: A single Arena owns a contiguous block of memory.  All allocations
 * within a command's lifetime are served from the arena; freeing is O(1)
 * (reset the watermark).  This avoids per-node HeapAlloc overhead in the
 * parser and matches ZSH's own region allocator strategy.
 *
 * SOLID: Single Responsibility — only memory lifecycle.
 *        Open/Closed — new allocation policies (e.g. aligned) extend via new
 *        functions; the struct is never modified.
 */
#ifndef WISP_ARENA_H
#define WISP_ARENA_H

#include <stddef.h>
#include "wisp_bool.h"


#ifdef __cplusplus
extern "C" {
#endif

/* Opaque handle; callers never touch internals directly (Encapsulation). */
typedef struct Arena Arena;

/* Create an arena with an initial capacity (bytes).
 * Returns NULL on allocation failure. */
Arena *arena_create(size_t initial_capacity);

/* Allocate 'size' zero-initialised bytes from the arena.
 * Grows automatically.  Never returns NULL (aborts on OOM). */
void  *arena_alloc(Arena *a, size_t size);

/* Duplicate a NUL-terminated string into the arena. */
char  *arena_strdup(Arena *a, const char *s);

/* Reset the watermark — all previous allocations become invalid.
 * Does NOT release the backing memory block (fast recycle). */
void   arena_reset(Arena *a);

/* Destroy the arena and release backing memory. */
void   arena_destroy(Arena *a);


#ifdef __cplusplus
}
#endif

#endif /* WISP_ARENA_H */
