#pragma once

#include "allocator.h"
#include "diag.h"
#include "module.h"
#include "object.h"
#include "sema.h"

// assign every module's non-generic functions, impl methods, structs and enums
// a program-wide slot (written back into each def's `slot`) and collect them
// into `exe`'s tables. must run over the whole registry before any module is
// compiled: a chunk may reference a definition from any module, and the heap
// roots off these tables. A generic definition gets no slot — it has no single
// body — and is reached only through the instances `Mono` derives from it.
// fails if the program outgrows the VM's one-byte operands (reported straight
// to stderr — the limit belongs to the program, not to any one declaration).
// `reg` must be topologically ordered.
bool exe_link(Executable *exe, ModuleRegistry *reg, Allocator *al);

// ── monomorphisation ─────────────────────────────────────────────────────────

// one compiled copy of a generic definition.
typedef struct {
  FunDef *origin;   // the generic definition, which owns the body AST
  FunDef *instance; // the copy that owns this instantiation's slot and chunk
  Subst subst;      // origin's type params (its impl's first) → concrete types
  int depth;        // instances traversed to get here; see MONO_MAX_DEPTH
} Instance;

// How far a chain of instantiations may run. `fun grow<T>(v: T) { grow([v]) }`
// type-checks but names a different instantiation at every level, so nothing
// downstream ever converges: each copy is keyed by a type one array deeper
// than the last. The one-byte slot space would stop it eventually, but only
// after interning a few hundred new types, and the type intern table is fixed
// — so the limit has to be here, where the divergence actually is.
#define MONO_MAX_DEPTH 32

// The monomorphisation queue. A generic definition is compiled once per
// distinct tuple of type arguments, discovered from the call sites codegen
// walks — so compiling one instance can enqueue more, and the caller must
// drain until `mono_pending_module` reports none left.
// the compile-time key a vtable is memoised under. Two coercions of the same
// concrete type to the same trait must reach the same `exe->vtables` entry —
// otherwise identical trait objects would carry different tables, and the
// slot space would grow with coercion *sites* rather than with (trait, type)
// pairs.
typedef struct {
  TraitDef *trait;
  Type *self_type; // interned, so pointer equality is identity
  int index;       // into exe->vtables
} DynVTable;

typedef struct {
  Executable *exe;
  Heap *heap;
  ImplIndex *impls; // to re-select an impl once a bound's receiver is concrete
  Allocator *al;

  Instance *insts;
  int count, cap;
  int compiled; // insts[0..compiled) already have chunks

  DynVTable *vtables;
  int vtable_count, vtable_cap;
} Mono;

void mono_init(Mono *mono, Executable *exe, Heap *heap, ImplIndex *impls,
               Allocator *al);

// compile every non-generic top-level function and impl method of `m` into
// bytecode (FunDef->chunk). string literals are interned into the heap as they
// are compiled; nested closures are appended to `exe->closures`; generic
// callees are enqueued on `mono` instead of being compiled in place. reports
// "not supported by the VM yet" diagnostics for constructs outside the
// executable subset. returns false on any error. requires exe_link first.
bool codegen_module(Module *m, Mono *mono, DiagBag *diags);

// the module owning the body of the next queued instantiation, so the caller
// can report its diagnostics against the right source. NULL once drained.
Module *mono_pending_module(Mono *mono);

// compile the next queued instantiation. returns false on any error.
bool mono_compile_next(Mono *mono, DiagBag *diags);
