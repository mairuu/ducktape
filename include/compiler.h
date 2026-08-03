#pragma once

#include "allocator.h"
#include "arena.h"
#include "diag.h"
#include "module.h"
#include "sema.h"

typedef struct {
  Arena arena;
  Allocator al;

  DiagBag diags;
  ModuleRegistry mod_reg;
  TypeChecker tc;

  Module *root_module;
} Compiler;

void compiler_init(Compiler *c, Allocator *al);

void compiler_destroy(Compiler *c, Allocator *al);

// run the full compilation pipeline over the program rooted at `path`: every
// module its tree declares, plus every library module they reach. `path`'s
// directory holds the root's children.
//
// `std_module` instead compiles the library alone, with the module that path
// names (`std::cmp`) read from disk and given the warning audience — the lint
// hatch. Exactly one of the two is non-NULL. Returns false if any phase failed.
bool compiler_run(Compiler *c, const char *path, const char *std_module);

// compile the checked root module to bytecode and run its `main`.
// call only after a successful compiler_run. `gc_stress` collects before
// every heap allocation instead of on the usual size threshold.
bool compiler_execute(Compiler *c, bool gc_stress);

// compile the checked program and write it to `out_path` as a bytecode image
// (`bytecode.h`) instead of running it. call only after a successful
// compiler_run.
bool compiler_emit(Compiler *c, const char *out_path);

// load a bytecode image and run it. independent of any Compiler — an image
// carries the whole linked program.
bool bytecode_execute(const char *path, bool gc_stress, Allocator *al);