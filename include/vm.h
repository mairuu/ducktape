#pragma once

#include "ast.h"
#include "object.h"

// execute `entry` (a compiled, zero-parameter function of the linked program
// `exe`), allocating heap objects (strings, arrays) through `heap`.
// returns false on a runtime error (reported to stderr).
bool vm_run(Executable *exe, Heap *heap, FunDef *entry);
