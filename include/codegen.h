#pragma once

#include "allocator.h"
#include "diag.h"
#include "module.h"
#include "object.h"

// assign every module's functions, impl methods, structs and enums a
// program-wide slot (written back into each def's `slot`) and collect them
// into `exe`'s tables. must run over the whole registry before any module is
// compiled: a chunk may reference a definition from any module, and the heap
// roots off these tables. fails if the program outgrows the VM's one-byte
// operands (reported straight to stderr — the limit belongs to the program,
// not to any one declaration). `reg` must be topologically ordered.
bool exe_link(Executable *exe, ModuleRegistry *reg, Allocator *al);

// compile every top-level function and impl method of `m` into bytecode
// (FunDef->chunk). string literals are interned into `heap` as they are
// compiled; nested closures are appended to `exe->closures`. reports
// "not supported by the VM yet" diagnostics for constructs outside the
// executable subset. returns false on any error. requires exe_link first.
bool codegen_module(Module *m, Executable *exe, Heap *heap, DiagBag *diags,
                    Allocator *al);
