#pragma once

#include "string_utils.h"
#include "value.h"

#include <stdbool.h>

typedef struct Heap Heap;
typedef struct Vm Vm;

// ═══════════════════════════════════════════════════════════════════════════════
// Natives — the C half of a bodyless std declaration
// ═══════════════════════════════════════════════════════════════════════════════
//
// A native's *signature* is written in ducktape (`@native("io_print") pub fun
// print<T>(value: T);`), so the checker needs no special path: it is an
// ordinary FunDef that happens to have a C body. C supplies only this
// name → function-pointer registry, and the name is what a bytecode image
// carries — a function pointer cannot be serialised, so `bc_load` re-binds
// each native against the running binary's registry.

// Everything a native may touch. `heap` is how it allocates; setting `error`
// to a static string turns the call into a runtime error at the call site,
// which is the only way a native can fail (the return value is otherwise
// whatever the ducktape signature promised).
typedef struct {
  Heap *heap;
  const char *error;

  // The running VM, for the natives that call back into ducktape. Only
  // `native_call` / `native_root` need it, and they take the context rather
  // than this field so the rest of the runtime stays out of a native's reach.
  Vm *vm;

  // Set when a call back into ducktape failed. The error was already reported
  // *there*, with the frames that were live when it happened, so the VM
  // unwinds without a second message. A native that sees `native_call` return
  // false must return promptly — having first dropped its roots.
  bool unwinding;
} NativeCtx;

// `args` points *into the VM's value stack* and stays live for the duration of
// the call, so an allocating native's arguments remain rooted across a
// collection it triggers. The result is pushed by the VM afterwards — a native
// may use the stack above `args + argc` only through `native_root`, and must
// leave it exactly as it found it.
typedef Value (*NativeFn)(NativeCtx *ctx, Value *args, int argc);

// ───────────────────────────────────────────────────────────────────────────────
// Calling ducktape from a native
// ───────────────────────────────────────────────────────────────────────────────
//
// The boundary in the other direction: a native that takes a function *value*
// (a comparator, a predicate) has to be able to run it. `native_call` builds
// the same call `OP_CALL` builds — callee, arguments, a frame — and drives the
// interpreter re-entrantly until that frame returns.
//
// Two rules come with it, and both follow from the callee being ordinary
// ducktape that can allocate:
//
//   1. **A collection can happen inside the call.** Anything the native is
//      holding must be reachable: its arguments are (they sit on the VM stack),
//      but a freshly built ObjArray in a C local is not, until `native_root`
//      puts it there.
//   2. **Any pointer *into* a heap object may be stale afterwards.** The callee
//      can push to the very array the native is walking, and
//      `heap_array_reserve` reallocates `items`. Re-read such pointers after
//      every call, or work on objects the program cannot reach.
//
// Returns false if the call failed, having already reported it and set
// `ctx->unwinding`; `*out` is untouched then.
bool native_call(NativeCtx *ctx, Value callee, const Value *args, int argc,
                 Value *out);

// Keep `v` alive until `native_unroot`, by putting it on the VM's stack — which
// is the root set, so a root is a push. Every root must be dropped before the
// native returns, including on the failure path.
void native_root(NativeCtx *ctx, Value v);
void native_unroot(NativeCtx *ctx, int count);

// look up `@native("name")`. NULL when the registry has no such entry.
NativeFn native_lookup(StringView name);

// look up `@intrinsic("name")`. Returns the OpCode the call lowers to, or -1.
int intrinsic_lookup(StringView name);

// comma-separated list of every registered name, for the "unknown native"
// diagnostic's note.
const char *native_names(void);
const char *intrinsic_names(void);
