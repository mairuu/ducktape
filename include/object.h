#pragma once

#include "allocator.h"
#include "value.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct Module Module;
typedef struct Heap Heap;
typedef struct StructDef StructDef;
typedef struct EnumDef EnumDef;
typedef struct VariantDef VariantDef;

// ═══════════════════════════════════════════════════════════════════════════════
// Heap objects
// ═══════════════════════════════════════════════════════════════════════════════

typedef enum {
  OBJ_STRING,
  OBJ_ARRAY,
  OBJ_TUPLE,
  OBJ_STRUCT,
  OBJ_ENUM,
} ObjKind;

// intrusive header shared by every heap object; `next` threads the heap's
// all-objects list for the sweep phase.
struct Obj {
  ObjKind kind;
  bool marked;
  Obj *next;
};

// interned: two equal strings are the same pointer, so `==` is pointer
// equality. NUL-terminated for printf convenience.
typedef struct {
  Obj obj;
  int len;
  uint32_t hash;
  char chars[];
} ObjString;

// fixed-size once built (no push operation in the language yet).
typedef struct {
  Obj obj;
  int count;
  Value *items;
} ObjArray;

// (a, b, c) — same shape as ObjArray, kept as a distinct ObjKind so
// printing/equality read as a tuple rather than an array.
typedef struct {
  Obj obj;
  int count;
  Value *items;
} ObjTuple;

// struct instance; `fields` holds `def->field_count` values in declaration
// order (not necessarily the initializer's source order).
typedef struct {
  Obj obj;
  StructDef *def;
  Value *fields;
} ObjStruct;

// enum variant instance; `fields` holds `variant->field_count` values in
// declaration order. `variant->tag` (assigned at codegen time) is what
// match compiles to a runtime tag test.
typedef struct {
  Obj obj;
  VariantDef *variant;
  Value *fields;
} ObjEnum;

static inline bool val_is_string(Value v) {
  return v.kind == VAL_OBJ && v.as.obj->kind == OBJ_STRING;
}
static inline ObjString *val_as_string(Value v) {
  return (ObjString *)v.as.obj;
}
static inline ObjArray *val_as_array(Value v) { return (ObjArray *)v.as.obj; }
static inline ObjTuple *val_as_tuple(Value v) { return (ObjTuple *)v.as.obj; }
static inline ObjStruct *val_as_struct(Value v) {
  return (ObjStruct *)v.as.obj;
}
static inline ObjEnum *val_as_enum(Value v) { return (ObjEnum *)v.as.obj; }

// ═══════════════════════════════════════════════════════════════════════════════
// Heap — owns every runtime object; mark-sweep collected
// ═══════════════════════════════════════════════════════════════════════════════

// extra roots beyond the module's chunk constants (the VM registers its
// value stack here while running).
typedef void (*HeapMarkRootsFn)(void *ctx);

struct Heap {
  Allocator al; // system allocator; objects are individually freed at sweep

  Obj *objects; // all objects, newest first

  ObjString **strings; // open-addressing intern table (weak: swept strings
  int string_count;    // are removed), tombstoned on deletion
  int string_cap;

  size_t bytes_allocated;
  size_t next_gc;
  bool stress; // collect before every allocation (--gc-stress)

  Module *module;             // compiled chunks' constant pools are roots
  HeapMarkRootsFn mark_roots; // NULL while no VM is running — allocation
  void *mark_roots_ctx;       // never triggers a collection then
};

void heap_init(Heap *h, Module *module, bool stress);
void heap_destroy(Heap *h);

// each constructor may trigger a collection *before* the new object exists,
// so every already-live operand must be reachable from a root (the VM keeps
// operands on its stack until the result is pushed).
ObjString *heap_intern(Heap *h, const char *chars, int len);
ObjString *heap_concat(Heap *h, ObjString *a, ObjString *b);
ObjArray *heap_array(Heap *h, int count); // items start as unit
ObjTuple *heap_tuple(Heap *h, int count); // items start as unit
ObjStruct *heap_struct(Heap *h, StructDef *def);
ObjEnum *heap_enum(Heap *h, VariantDef *variant);

void gc_mark_value(Value v);
void heap_collect(Heap *h);
