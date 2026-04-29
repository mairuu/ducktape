#include "allocator.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void *_allocate_zero(Allocator *al, size_t size) {
  void *ptr = al_alloc(al, size);
  if (ptr) {
    memset(ptr, 0, size);
  }
  return ptr;
}

static void *_heap_alloc(void *ctx, size_t size) {
  (void)ctx; // unused
  return malloc(size);
}

static void *_heap_realloc(void *ctx, void *ptr, size_t old_size, size_t new_size) {
  (void)ctx;
  (void)old_size;
  return realloc(ptr, new_size);
}

static void _heap_free(void *ctx, void *ptr, size_t size) {
  (void)ctx;
  (void)size;
  free(ptr);
}

Allocator heap_allocator_create(void) {
  return (Allocator){
      .alloc = _heap_alloc,
      .realloc = _heap_realloc,
      .free = _heap_free,
      .ctx = NULL,
  };
}

#define INITIAL_CAPACITY 64
static_assert((INITIAL_CAPACITY & (INITIAL_CAPACITY - 1)) == 0, "capacity must be power of 2");

#define GROWTH_FACTOR 2

#define TOMBSTONE ((void *)-1)

#define ENTRY_ALIVE(e) ((e)->ptr != NULL && (e)->ptr != TOMBSTONE)

static size_t _ptr_hash(void *ptr, size_t cap) {
  uintptr_t h = (uintptr_t)ptr;
  h ^= h >> 16;
  h *= 0x45d9f3b;
  h ^= h >> 16;
  return h & (cap - 1);
}

static void _track_insert(TrackEntry *entries, size_t cap, void *ptr, size_t size) {
  size_t i = _ptr_hash(ptr, cap);
  while (ENTRY_ALIVE(&entries[i])) {
    i = (i + 1) & (cap - 1);
  }
  entries[i] = (TrackEntry){.ptr = ptr, .size = size};
}

static TrackEntry *_track_find(TrackEntry *entries, size_t cap, void *ptr) {
  size_t i = _ptr_hash(ptr, cap);
  while (entries[i].ptr != NULL) {
    if (entries[i].ptr == ptr) {
      return &entries[i];
    }
    i = (i + 1) & (cap - 1);
  }
  return NULL;
}

// grow 2x when load factor exceeds 0.6
static void _maybe_grow(TrackAllocator *ta) {
  if (ta->count * 10 < ta->capacity * 6) {
    return;
  }

  size_t new_cap = ta->capacity * GROWTH_FACTOR;
  TrackEntry *new_entries = al_alloc_zero(ta->inner, new_cap * sizeof(TrackEntry));

  for (size_t i = 0; i < ta->capacity; i++) {
    if (ENTRY_ALIVE(&ta->entries[i])) {
      _track_insert(new_entries, new_cap, ta->entries[i].ptr, ta->entries[i].size);
    }
  }
  al_free(ta->inner, ta->entries, ta->capacity * sizeof(TrackEntry));
  ta->entries = new_entries;
  ta->capacity = new_cap;
}

static void *_track_alloc(void *ctx, size_t size) {
  TrackAllocator *ta = ctx;
  void *ptr = al_alloc(ta->inner, size);
  if (!ptr) {
    assert(false);
    return NULL;
  }

  _maybe_grow(ta);
  _track_insert(ta->entries, ta->capacity, ptr, size);
  ta->count++;
  ta->total_allocated += size;
#ifdef DEBUG_VERBOSE_TRACK_ALLOC
  fprintf(stderr, "[track_alloc] alloc: ptr=%p size=%zu\n", ptr, size);
#endif
  return ptr;
}

static void *_track_realloc(void *ctx, void *ptr, size_t old_size, size_t new_size) {
  TrackAllocator *ta = ctx;

  if (ptr) {
    TrackEntry *e = _track_find(ta->entries, ta->capacity, ptr);
    if (e) {
      if (e->size != old_size) {
        fprintf(stderr,
                "[track_alloc] size mismatch on realloc: ptr=%p "
                "tracked=%zu caller=%zu\n",
                ptr, e->size, old_size);
      }
      e->ptr = TOMBSTONE;
      ta->count--;
    } else {
      fprintf(stderr, "[track_alloc] realloc of untracked ptr %p (old_size=%zu)\n", ptr, old_size);
    }
  }

  void *new_ptr = al_realloc(ta->inner, ptr, old_size, new_size);
  if (!new_ptr) {
    assert(false);
    return NULL;
  }

  _maybe_grow(ta);
  _track_insert(ta->entries, ta->capacity, new_ptr, new_size);
  ta->count++;

  if (new_size > old_size) {
    ta->total_allocated += new_size - old_size;
  } else {
    ta->total_freed += old_size - new_size;
  }

#ifdef DEBUG_VERBOSE_TRACK_ALLOC
  fprintf(stderr, "[track_alloc] realloc: old_ptr=%p old_size=%zu new_ptr=%p new_size=%zu\n", ptr,
          old_size, new_ptr, new_size);
#endif

  return new_ptr;
}

static void _track_free(void *ctx, void *ptr, size_t size) {
  TrackAllocator *ta = ctx;
  if (!ptr) {
    return;
  }

  TrackEntry *e = _track_find(ta->entries, ta->capacity, ptr);
  if (!e) {
    fprintf(stderr, "[track_alloc] free of untracked ptr %p (size %zu)\n", ptr, size);
  } else {
    if (e->size != size) {
      fprintf(stderr,
              "[track_alloc] size mismatch on free: ptr=%p "
              "tracked=%zu caller=%zu\n",
              ptr, e->size, size);
    }
    e->ptr = TOMBSTONE;
    ta->count--;
    ta->total_freed += size;
  }
  al_free(ta->inner, ptr, size);

#ifdef DEBUG_VERBOSE_TRACK_ALLOC
  fprintf(stderr, "[track_alloc] free: ptr=%p size=%zu\n", ptr, size);
#endif
}

void track_alloc_init(TrackAllocator *ta, Allocator *inner) {
  ta->inner = inner;
  ta->entries = al_alloc_zero(inner, INITIAL_CAPACITY * sizeof(TrackEntry));
  ta->capacity = INITIAL_CAPACITY;
  ta->count = 0;
  ta->total_allocated = 0;
  ta->total_freed = 0;
}

#define VERBOSE_LEAK_REPORT 1

void track_alloc_report(TrackAllocator *ta) {
  size_t leaks = 0;
  for (size_t i = 0; i < ta->capacity; i++) {
    if (ENTRY_ALIVE(&ta->entries[i])) {
#ifdef VERBOSE_LEAK_REPORT
      fprintf(stderr, "[track_alloc] leak: ptr=%p size=%zu\n", ta->entries[i].ptr,
              ta->entries[i].size);
#endif
      leaks++;
    }
  }
  if (leaks == 0) {
    fprintf(stderr,
            "[track_alloc] no leaks detected. "
            "allocated=%zu freed=%zu\n",
            ta->total_allocated, ta->total_freed);
  } else {
    fprintf(stderr,
            "[track_alloc] %zu leak(s) found. "
            "allocated=%zu freed=%zu\n",
            leaks, ta->total_allocated, ta->total_freed);
  }
}

void track_alloc_destroy(TrackAllocator *ta) {
  al_free(ta->inner, ta->entries, ta->capacity * sizeof(TrackEntry));
  ta->entries = NULL;
}

Allocator track_allocator_create(TrackAllocator *ta) {
  return (Allocator){
      .alloc = _track_alloc,
      .realloc = _track_realloc,
      .free = _track_free,
      .ctx = ta,
  };
}