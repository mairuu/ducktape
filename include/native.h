#pragma once

#include "string_utils.h"
#include "value.h"

#include <stdbool.h>

typedef struct Heap Heap;

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
} NativeCtx;

// `args` points *into the VM's value stack* and stays live for the duration of
// the call, so an allocating native's arguments remain rooted across a
// collection it triggers. The result is pushed by the VM afterwards — a native
// must never write above `args + argc`.
typedef Value (*NativeFn)(NativeCtx *ctx, Value *args, int argc);

// look up `@native("name")`. NULL when the registry has no such entry.
NativeFn native_lookup(StringView name);

// look up `@intrinsic("name")`. Returns the OpCode the call lowers to, or -1.
int intrinsic_lookup(StringView name);

// comma-separated list of every registered name, for the "unknown native"
// diagnostic's note.
const char *native_names(void);
const char *intrinsic_names(void);
