#pragma once

#include <stddef.h>

// The one contract every implementation owes its callers, beyond the obvious:
// **a zero-size request still returns a valid pointer**, not NULL. It may not
// be dereferenced, but it may be handed to `memcpy`/`memcmp`/`memset` with a
// length of zero — which those functions require even then, and which is
// exactly how a zero-length allocation is normally used:
//
//     T *xs = al_alloc(al, sizeof(T) * n);
//     memcpy(xs, src, sizeof(T) * n);       // correct for every n, 0 included
//
// Answering NULL instead would make that natural spelling undefined behaviour
// for `n == 0` and push a guard onto every caller, forever. It also keeps NULL
// meaning one thing — allocation failed — rather than two.
typedef struct {
  void *(*alloc)(void *ctx, size_t size);
  void *(*realloc)(void *ctx, void *ptr, size_t old_size, size_t new_size);
  void (*free)(void *ctx, void *ptr, size_t size);
  void *ctx;
} Allocator;

#define al_alloc(al, size) (al)->alloc((al)->ctx, size)

#define al_alloc_for(al, type) (type *)al_alloc(al, sizeof(type))

#define al_alloc_zero(al, size) _allocate_zero(al, size)

#define al_alloc_zero_for(al, type) (type *)al_alloc_zero(al, sizeof(type))

#define al_realloc(al, ptr, old_size, new_size)                                \
  (al)->realloc((al)->ctx, ptr, old_size, new_size)

#define al_free(al, ptr, size) (al)->free((al)->ctx, ptr, size)

#define al_free_for(al, ptr) (al)->free((al)->ctx, ptr, sizeof((*ptr)))

void *_allocate_zero(Allocator *al, size_t size);

Allocator heap_allocator_create(void);
