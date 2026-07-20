#pragma once

#include "allocator.h"
#include "diag.h"
#include "module.h"
#include "object.h"

// compile every top-level function of `m` into bytecode (FunDef->chunk) and
// assign global slots. string literals are interned into `heap` as they are
// compiled. reports "not supported by the VM yet" diagnostics for
// constructs outside the executable subset. returns false on any error.
bool codegen_module(Module *m, Heap *heap, DiagBag *diags, Allocator *al);
