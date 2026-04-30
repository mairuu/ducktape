#include "garbage_collector.h"

#include "array.h"
#include "object.h"
#include "virtual_machine.h"

#include <assert.h>

#ifdef DEBUG_LOG_GC
#include <stdio.h>
#endif

#define GC_INITIAL_THRESHOLD (1024 * 1024) // 1MB

static void gc_mark_value(GarbageCollector *gc, Value value);

static void gc_mark_hash_table(GarbageCollector *gc, HashTable *table) {
  HashTableIterator it;
  hti_init(&it, table);
  while (hti_next(&it)) {
    gc_mark_value(gc, *it.key);
    gc_mark_value(gc, *it.value);
  }
}

static void ht_remove_unmarked(HashTable *table) {
  HashTableIterator it;
  hti_init(&it, table);
  while (hti_next(&it)) {
    if (IS_OBJECT(*it.key) && !obj_is_marked(AS_OBJECT(*it.key))) {
      ht_delete(table, *it.key);
    }
  }
}

static void gc_mark_object(GarbageCollector *gc, Object *obj) {
  if (obj == NULL || obj_is_marked(obj)) {
    return;
  }

#ifdef DEBUG_LOG_GC
  printf("%p mark ", (void *)obj);
  obj_print(obj);
  printf("\n");
#endif

  obj_set_marked(obj, true);
  arr_push(gc->gray_stack, obj, gc->inner);
}

static void gc_mark_value(GarbageCollector *gc, Value value) {
  if (IS_OBJECT(value)) {
    gc_mark_object(gc, AS_OBJECT(value));
  }
}

static void gc_mark_values(GarbageCollector *gc, Value *values, size_t count) {
  for (size_t i = 0; i < count; i++) {
    gc_mark_value(gc, values[i]);
  }
}

static void gc_mark_roots(GarbageCollector *gc) {
  for (Value *slot = gc->vm->stack; slot < gc->vm->stack_top; slot++) {
    gc_mark_value(gc, *slot);
  }

  gc_mark_hash_table(gc, &gc->vm->globals);
}

static void gc_blacken_object(GarbageCollector *gc, Object *obj) {
#ifdef DEBUG_LOG_GC
  printf("%p blacken ", (void *)obj);
  obj_print(obj);
  printf("\n");
#endif
  switch (obj->type) {
  case OBJ_STRING:
    break;
  case OBJ_FUNCTION: {
    ObjFunction *func = (ObjFunction *)obj;
    gc_mark_object(gc, &func->name->obj);
    gc_mark_values(gc, func->chunk.constants->data, func->chunk.constants->header.count);
    break;
  }
  case OBJ_NATIVE:
    break;
  default:
    assert(false && "unknown object type");
    break;
  }
}

static void gc_trace_references(GarbageCollector *gc) {
  while (arr_count(gc->gray_stack) > 0) {
    gc_blacken_object(gc, arr_pop(gc->gray_stack));
  }
}

static void gc_sweep(GarbageCollector *gc) {
  Object *prev = NULL;
  Object *obj = gc->vm->objects;
  while (obj != NULL) {
    if (obj_is_marked(obj)) {
      obj_set_marked(obj, false);
      prev = obj;
      obj = obj->next;
      continue;
    }

    // obj is unreachable
    Object *unreached = obj;
    obj = obj->next;
    if (prev != NULL) {
      prev->next = obj;
    } else {
      gc->vm->objects = obj;
    }
    obj_free(&unreached, &gc->as_allocator);
  }
}

static void gc_collect(GarbageCollector *gc) {
  if (gc->vm->gc_disabled) {
    return;
  }

#ifdef DEBUG_LOG_GC
  printf("-- gc begin\n");
  size_t before = gc->bytes_allocated;
#endif

  gc_mark_roots(gc);
  gc_trace_references(gc);
  ht_remove_unmarked(&gc->vm->strings);
  gc_sweep(gc);

  gc->next_gc = gc->bytes_allocated * 2;

#ifdef DEBUG_LOG_GC
  printf("-- gc end\n");
  printf("   collected %zu bytes (from %zu to %zu) next at %zu\n", before - gc->bytes_allocated,
         before, gc->bytes_allocated, gc->next_gc);
#endif
}

static void *_gc_alloc(void *ctx, size_t size) {
  GarbageCollector *gc = (GarbageCollector *)ctx;
#ifdef DEBUG_STRESS_GC
  gc_collect(gc);
#endif
  gc->bytes_allocated += size;
  if (gc->bytes_allocated >= gc->next_gc) {
    gc_collect(gc);
  }
  return al_alloc(gc->inner, size);
}

static void *_gc_realloc(void *ctx, void *ptr, size_t old_size, size_t new_size) {
  GarbageCollector *gc = (GarbageCollector *)ctx;
#ifdef DEBUG_STRESS_GC
  gc_collect(gc);
#endif
  gc->bytes_allocated += new_size - old_size;
  return al_realloc(gc->inner, ptr, old_size, new_size);
}

static void _gc_free(void *ctx, void *ptr, size_t size) {
  GarbageCollector *gc = (GarbageCollector *)ctx;
  gc->bytes_allocated -= size;
  al_free(gc->inner, ptr, size);
}

void gc_init(GarbageCollector *gc, VirtualMachine *vm, Allocator *inner) {
  gc->bytes_allocated = 0;
  gc->next_gc = GC_INITIAL_THRESHOLD;
  gc->inner = inner;
  gc->vm = vm;
  gc->gray_stack = arr_new();
  gc->as_allocator = (Allocator){
      .alloc = _gc_alloc,
      .realloc = _gc_realloc,
      .free = _gc_free,
      .ctx = gc,
  };
}

void gc_destroy(GarbageCollector *gc) { arr_free(&gc->gray_stack, gc->inner); }

Allocator gc_allocator_create(GarbageCollector *gc) { return gc->as_allocator; }
