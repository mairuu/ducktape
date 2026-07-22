#pragma once

#include "allocator.h"
#include "value.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct Heap Heap;
typedef struct StructDef StructDef;
typedef struct EnumDef EnumDef;
typedef struct VariantDef VariantDef;

// ═══════════════════════════════════════════════════════════════════════════════
// VTable — the dynamic half of a trait object
// ═══════════════════════════════════════════════════════════════════════════════

// One entry per method the trait declares, in declaration order, each already
// resolved (and monomorphised) for one concrete self type. This is exactly the
// choice monomorphisation makes at compile time, deferred: a `dyn Show` value
// carries the table its concrete type would have selected, so `OP_DYN_METHOD`
// can index it by the position the trait fixes.
//
// Note what is *not* here: the trait and the self type it was built for. Those
// are the compile-time memo key (`Mono.vtables`), not something the VM reads —
// the same rule the serialization milestone set, that an image is the runtime
// projection of the program rather than a snapshot of the compiler.
typedef struct {
  FunDef **methods;
  int method_count;
} VTable;

// ═══════════════════════════════════════════════════════════════════════════════
// Executable — the linked program: every module's definitions in one slot space
// ═══════════════════════════════════════════════════════════════════════════════

// OP_GET_GLOBAL/OP_STRUCT/OP_ENUM each carry a single operand byte, and a
// chunk compiled from one module routinely names a definition from another
// (an imported function, a struct built by its constructor), so the operand
// cannot mean "index into my module". These are the program-wide tables those
// operands index; codegen only ever emits `def->slot`, whichever module the
// def came from.
//
// Every one of them is filled *on demand*, as codegen discovers references —
// `exe_link` only sizes them. A definition nothing reaches never enters a
// table and keeps `SLOT_NONE`, so what a program spends of the one-byte
// operand space is a property of what it uses rather than of what its imports
// happen to declare. Generic definitions have always behaved this way (they
// have no single body to address, so only their instances are entered); the
// rest of the program simply follows the same rule.
typedef struct {
  // OP_GET_GLOBAL operand space: top-level funs, impl methods and
  // monomorphised instances alike, in the order the walk from `main` reaches
  // them.
  FunDef **globals;
  int global_count;
  int global_cap;

  StructDef **structs; // OP_STRUCT operand space
  int struct_count;
  int struct_cap;

  EnumDef **enums; // OP_ENUM operand space
  int enum_count;
  int enum_cap;

  // nested closure functions, appended by codegen. Not addressable by any
  // opcode (they're built at runtime via OP_CLOSURE, from a chunk constant),
  // but their chunks' constant pools still need to be GC roots.
  FunDef **closures;
  int closure_count, closure_cap;

  // OP_MAKE_DYN operand space. Appended by codegen as coercion sites are
  // discovered, like the monomorphised globals above and for the same reason:
  // which (trait, type) pairs a program needs is a property of its call sites.
  VTable **vtables;
  int vtable_count, vtable_cap;
} Executable;

// ═══════════════════════════════════════════════════════════════════════════════
// Heap objects
// ═══════════════════════════════════════════════════════════════════════════════

typedef enum {
  OBJ_STRING,
  OBJ_ARRAY,
  OBJ_TUPLE,
  OBJ_STRUCT,
  OBJ_ENUM,
  OBJ_CLOSURE,
  OBJ_UPVALUE,
  OBJ_DYN,
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

// growable: `count` live values inside a buffer of `cap`. A literal is built
// exact-fit (`OP_ARRAY` knows how many elements it just evaluated) and only
// `std::array::push` ever grows one, so a program that never pushes pays
// nothing for the capacity field but its four bytes.
//
// `count` is the whole of what is live: the mark phase walks exactly
// `[0, count)` and the slots past it are never read, which is why growth may
// only raise `count` *after* the slot it names exists.
typedef struct {
  Obj obj;
  int count;
  int cap;
  Value *items;
} ObjArray;

// (a, b, c) — same shape as ObjArray minus the capacity (a tuple's length is
// part of its type, so it can never grow), kept as a distinct ObjKind so
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

// a captured variable. while the closure's defining frame is still live the
// upvalue is "open" — `location` points at the variable's stack slot, shared
// so writes are seen through both the slot and the upvalue. when that slot
// dies (scope/function exit) the upvalue is "closed": the value is copied into
// `closed` and `location` is repointed at it, so the closure keeps working
// after the frame is gone. `next` threads the VM's open-upvalue list.
typedef struct ObjUpvalue {
  Obj obj;
  Value *location;
  Value closed;
  struct ObjUpvalue *next;
} ObjUpvalue;

// a function value that closes over variables: the compiled `fun` plus the
// concrete upvalue cells captured at the point the closure expression ran.
// non-capturing functions (top-level, methods) stay plain `VAL_FUN` values.
typedef struct {
  Obj obj;
  FunDef *fun;
  ObjUpvalue **upvalues;
  int upvalue_count;
} ObjClosure;

// a trait object: the value that was coerced, plus the vtable of the impl it
// was coerced through. The inner value is stored as-is — coercion wraps, it
// never converts — so `OP_DYN_METHOD` hands the method exactly the receiver it
// was compiled for.
typedef struct {
  Obj obj;
  Value inner;
  VTable *vtable;
} ObjDyn;

static inline bool val_is_string(Value v) {
  return v.kind == VAL_OBJ && v.as.obj->kind == OBJ_STRING;
}
static inline bool val_is_closure(Value v) {
  return v.kind == VAL_OBJ && v.as.obj->kind == OBJ_CLOSURE;
}
static inline ObjClosure *val_as_closure(Value v) {
  return (ObjClosure *)v.as.obj;
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
static inline bool val_is_dyn(Value v) {
  return v.kind == VAL_OBJ && v.as.obj->kind == OBJ_DYN;
}
static inline ObjDyn *val_as_dyn(Value v) { return (ObjDyn *)v.as.obj; }

// ═══════════════════════════════════════════════════════════════════════════════
// Heap — owns every runtime object; mark-sweep collected
// ═══════════════════════════════════════════════════════════════════════════════

// extra roots beyond the program's chunk constants (the VM registers its
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

  Executable *exe;            // compiled chunks' constant pools are roots
  HeapMarkRootsFn mark_roots; // NULL while no VM is running — allocation
  void *mark_roots_ctx;       // never triggers a collection then
};

// `exe` must already be linked (its tables sized and filled) when the heap is
// created: codegen interns strings as it compiles, so a collection can happen
// with only some chunks filled in — a FunDef whose chunk is still NULL is a
// root with nothing to mark, but one missing from the tables is not a root at
// all.
void heap_init(Heap *h, Executable *exe, bool stress);
void heap_destroy(Heap *h);

// each constructor may trigger a collection *before* the new object exists,
// so every already-live operand must be reachable from a root (the VM keeps
// operands on its stack until the result is pushed).
ObjString *heap_intern(Heap *h, const char *chars, int len);
ObjString *heap_concat(Heap *h, ObjString *a, ObjString *b);
ObjArray *heap_array(Heap *h, int count); // items start as unit; cap == count
ObjTuple *heap_tuple(Heap *h, int count); // items start as unit
ObjStruct *heap_struct(Heap *h, StructDef *def);
ObjEnum *heap_enum(Heap *h, VariantDef *variant);
ObjClosure *heap_closure(Heap *h, FunDef *fun, int upvalue_count);
ObjUpvalue *heap_upvalue(Heap *h, Value *slot);
ObjDyn *heap_dyn(Heap *h, Value inner, VTable *vtable);

// widen `arr` so it can hold `needed` values, doubling its buffer. Like every
// constructor above it may collect, so `arr` itself must already be reachable
// from a root — which for `std::array::push` it is, being an argument still
// sitting on the VM stack.
void heap_array_reserve(Heap *h, ObjArray *arr, int needed);

void gc_mark_value(Value v);
void heap_collect(Heap *h);
