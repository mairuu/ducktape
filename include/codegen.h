#pragma once

#include "allocator.h"
#include "diag.h"
#include "module.h"

// compile every top-level function of `m` into bytecode (FunDef->chunk) and
// assign global slots. reports "not supported by the VM yet" diagnostics for
// constructs outside the executable subset. returns false on any error.
bool codegen_module(Module *m, DiagBag *diags, Allocator *al);
