#pragma once

#include <stddef.h>

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

#define al_realloc(al, ptr, old_size, new_size) (al)->realloc((al)->ctx, ptr, old_size, new_size)

#define al_free(al, ptr, size) (al)->free((al)->ctx, ptr, size)

#define al_free_for(al, ptr) (al)->free((al)->ctx, ptr, sizeof((*ptr)))

void *_allocate_zero(Allocator *al, size_t size);

Allocator heap_allocator_create(void);

typedef struct {
  void *ptr;
  size_t size;
} TrackEntry;

typedef struct {
  Allocator *inner;
  TrackEntry *entries;
  size_t capacity;
  size_t count;
  size_t total_allocated;
  size_t total_freed;
} TrackAllocator;

void track_alloc_init(TrackAllocator *ta, Allocator *inner);

void track_alloc_destroy(TrackAllocator *ta);

void track_alloc_report(TrackAllocator *ta);

Allocator track_allocator_create(TrackAllocator *ctx);