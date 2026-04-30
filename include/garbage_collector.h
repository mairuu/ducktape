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

  int disabled;

  Allocator *inner;
  Allocator as_allocator;
} GarbageCollector;

void gc_init(GarbageCollector *gc, VirtualMachine *vm, Allocator *inner);

void gc_destroy(GarbageCollector *gc);

// disable gc until end_staging is called
void gc_begin_staging(GarbageCollector *gc);

// reenable gc and collect if needed
void gc_end_staging(GarbageCollector *gc);

// collect if bytes allocated exceeds threshold
// no-op when gc is disabled (in staging)
void gc_collect_if_needed(GarbageCollector *gc);

Allocator gc_allocator_create(GarbageCollector *gc);
