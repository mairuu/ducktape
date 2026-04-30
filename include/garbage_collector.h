#pragma once

#include <stddef.h>

#include "allocator.h"
#include "array.h"

typedef struct Object Object;
typedef struct VirtualMachine VirtualMachine;

DEFINE_ARRAY(Object *, ObjectPtrArray);

typedef struct GarbageCollector {
  VirtualMachine *vm;

  ObjectPtrArray *gray_stack;

  size_t bytes_allocated;
  size_t next_gc;

  Allocator *inner;
  Allocator as_allocator;
} GarbageCollector;

void gc_init(GarbageCollector *gc, VirtualMachine *vm, Allocator *inner);

void gc_destroy(GarbageCollector *gc);

Allocator gc_allocator_create(GarbageCollector *gc);
