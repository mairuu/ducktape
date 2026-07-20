#include "allocator.h"

#include <stdlib.h>
#include <string.h>

void *_allocate_zero(Allocator *al, size_t size) {
  if (size == 0) {
    return NULL;
  }
  void *ptr = al_alloc(al, size);
  if (ptr) {
    memset(ptr, 0, size);
  }
  return ptr;
}

static void *_heap_alloc(void *ctx, size_t size) {
  (void)ctx; // unused
  if (size == 0) {
    return NULL;
  }
  return malloc(size);
}

static void *_heap_realloc(void *ctx, void *ptr, size_t old_size,
                           size_t new_size) {
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
