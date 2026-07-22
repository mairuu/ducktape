#pragma once

#include "allocator.h"
#include "diag.h"
#include "module.h"
#include "object.h"
#include "sema.h"

// prepare `exe` for codegen: size the three slot spaces (all empty — slots are
// handed out as codegen reaches definitions, see `mono_seed`) and fix every
// enum's variant tags, which are not slots and so cannot wait for a reference.
// must run before any module is compiled: the heap roots off these tables.
void exe_link(Executable *exe, ModuleRegistry *reg, Allocator *al);

// ── monomorphisation ─────────────────────────────────────────────────────────

// one body queued for compilation. For a generic definition that is one copy
// per distinct tuple of type arguments; for a non-generic one it is the
// definition itself (`origin == instance`, `subst` empty), since there is
// nothing to specialise and so no reason to copy.
typedef struct {
  FunDef *origin;   // the definition, which owns the body AST
  FunDef *instance; // what owns this copy's slot and chunk
  Subst subst;      // origin's type params (its impl's first) → concrete types
  int depth;        // instances traversed to get here; see MONO_MAX_DEPTH

  // the impl set to select from while compiling this body. For an
  // instantiation it is the *requesting* module's, not the defining module's:
  // `std::cmp::max<T: Ord>` needs `impl Ord for P`, and that impl is written
  // where max is called — the witness for a bound travels with the
  // instantiation, exactly as its type arguments do (`subst` above). A
  // requester always reaches every module along the chain it started, so its
  // set is a superset of every set the bodies below it could need. A
  // non-generic body has no bound to witness, so it uses its own module's set
  // — the one the checker used on it — and who reached it first cannot matter.
  ImplIndex *impls;
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
  Allocator *al;

  Instance *insts;
  int count, cap;
  int compiled; // insts[0..compiled) already have chunks

  DynVTable *vtables;
  int vtable_count, vtable_cap;
} Mono;

void mono_init(Mono *mono, Executable *exe, Heap *heap, Allocator *al);

// give the entry point a slot and queue its body — the root of the walk, and
// the only definition anything outside codegen names. Everything else in the
// program arrives through what `entry` reaches, transitively. `entry` must be
// neither generic nor native (it needs a chunk of its own). false if the
// program has already outgrown the globals space, which needs one function to
// do. requires exe_link first.
bool mono_seed(Mono *mono, FunDef *entry);

// the module owning the body of the next queued definition, so the caller can
// report its diagnostics against the right source. NULL once drained.
Module *mono_pending_module(Mono *mono);

// compile the next queued body into bytecode (FunDef->chunk). string literals
// are interned into the heap as it goes; nested closures are appended to
// `exe->closures`; every definition it references is slotted and queued behind
// it. reports "not supported by the VM yet" for constructs outside the
// executable subset. returns false on any error.
bool mono_compile_next(Mono *mono, DiagBag *diags);
