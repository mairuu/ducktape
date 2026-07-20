#pragma once

#include "ast.h"
#include "module.h"
#include "object.h"

// execute `entry` (a compiled, zero-parameter function of `m`), allocating
// heap objects (strings, arrays) through `heap`.
// returns false on a runtime error (reported to stderr).
bool vm_run(Module *m, Heap *heap, FunDef *entry);
