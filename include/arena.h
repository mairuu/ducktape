#pragma once

#include "allocator.h"
#include <stddef.h>

#define ARENA_BLOCK_SIZE (64 * 1024) // 64 KiB

typedef struct ArenaBlock {
  struct ArenaBlock *next;
  size_t capacity;
  size_t used;
  char data[];
} ArenaBlock;

typedef struct {
  ArenaBlock *current;
  ArenaBlock *first;
  Allocator *inner;
} Arena;

void arena_init(Arena *arena, Allocator *inner);

void arena_free(Arena *arena);

void arena_reset(Arena *arena);

Allocator arena_allocator_create(Arena *arena);