#pragma once

#include "ast.h"
#include "module.h"

// execute `entry` (a compiled, zero-parameter function of `m`).
// returns false on a runtime error (reported to stderr).
bool vm_run(Module *m, FunDef *entry);
