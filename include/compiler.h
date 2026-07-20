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

// run the full compilation pipeline
// only single file for now
// returns false if any phase failed
bool compiler_run(Compiler *c, const char *path);

// compile the checked root module to bytecode and run its `main`.
// call only after a successful compiler_run.
bool compiler_execute(Compiler *c);