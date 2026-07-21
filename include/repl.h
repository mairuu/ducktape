#pragma once

#include "allocator.h"

#include <stdbool.h>

// read-eval-print loop over stdin. Returns false only if the session could not
// be started; a line that fails to compile or traps at runtime is reported and
// the session continues.
bool repl_run(Allocator *al, bool gc_stress);
