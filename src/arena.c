#include "arena.h"
#include "allocator.h"

#include <string.h>

// ── helpers ──────────────────────────────────────────────────────────────────

static ArenaBlock *block_new(Allocator *inner, size_t min_size) {
  size_t capacity = ARENA_BLOCK_SIZE;
  if (min_size > capacity) {
    capacity = min_size;
  }

  size_t total = sizeof(ArenaBlock) + capacity;
  ArenaBlock *b = al_alloc(inner, total);
  if (!b)
    return NULL;

  b->next = NULL;
  b->capacity = capacity;
  b->used = 0;
  return b;
}

// ── align up to pointer-size boundary ────────────────────────────────────────

static size_t align_up(size_t n) {
  const size_t align = _Alignof(max_align_t);
  return (n + align - 1) & ~(align - 1);
}

// ── Allocator vtable callbacks
// ────────────────────────────────────────────────

static void *arena_alloc_fn(void *ctx, size_t size) {
  Arena *a = ctx;
  // a zero-size request still gets an address of its own — one alignment unit,
  // since `align_up(0)` is 0 and a bump of nothing would hand out the *next*
  // allocation's address. See the contract in allocator.h: NULL means failure
  // here and nothing else.
  size = size == 0 ? align_up(1) : align_up(size);

  if (a->current && a->current->used + size <= a->current->capacity) {
    void *ptr = a->current->data + a->current->used;
    a->current->used += size;
    return ptr;
  }

  ArenaBlock *b = block_new(a->inner, size);
  if (!b) {
    return NULL;
  }

  // link at the front for O(1) insertion; first stays fixed for arena_free
  b->next = a->current ? a->current->next : NULL;
  if (a->current)
    a->current->next = b;
  else
    a->first = b;
  a->current = b;

  void *ptr = b->data;
  b->used = size;
  return ptr;
}

static void *arena_realloc_fn(void *ctx, void *ptr, size_t old_size,
                              size_t new_size) {
  void *fresh = arena_alloc_fn(ctx, new_size);
  if (!fresh)
    return NULL;
  if (ptr && old_size)
    memcpy(fresh, ptr, old_size < new_size ? old_size : new_size);
  return fresh;
}

static void arena_free_fn(void *ctx, void *ptr, size_t size) {
  (void)ctx;
  (void)ptr;
  (void)size;
  // intentional no-op — bulk-freed by arena_free()
}

// ── public API
// ────────────────────────────────────────────────────────────────

void arena_init(Arena *arena, Allocator *inner) {
  arena->current = NULL;
  arena->first = NULL;
  arena->inner = inner;
}

void arena_destroy(Arena *arena) {
  ArenaBlock *b = arena->first;
  while (b) {
    ArenaBlock *next = b->next;
    al_free(arena->inner, b, sizeof(ArenaBlock) + b->capacity);
    b = next;
  }
  arena->first = NULL;
  arena->current = NULL;
}

void arena_reset(Arena *arena) {
  for (ArenaBlock *b = arena->first; b; b = b->next) {
    b->used = 0;
  }
  arena->current = arena->first;
}

Allocator arena_allocator_create(Arena *arena) {
  return (Allocator){
      .alloc = arena_alloc_fn,
      .realloc = arena_realloc_fn,
      .free = arena_free_fn,
      .ctx = arena,
  };
}
