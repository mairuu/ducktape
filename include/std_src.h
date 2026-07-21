#pragma once

#include "string_utils.h"

// The standard library is written in ducktape (`std/*.dt`) and mirrored into
// the binary by `scripts/embed_std.sh`, so `use std::cmp;` resolves with no
// install path, no env var and no search directory — which keeps the test
// suite hermetic and a bytecode image self-contained.
//
// This is the *only* place that knows std source doesn't come from a file.
// `mod_parse` asks here; everything else — the registry, the dependency graph,
// cycle detection, `pub`, linking — treats a std module as an ordinary one.
// Moving std to a real directory later means changing that one branch.

// source text of the embedded std module `name`, or NULL if there is none.
const char *std_module_source(StringView name);

// the embedded module names as `a, b, c`, for the unknown-module diagnostic.
const char *std_module_names(void);
