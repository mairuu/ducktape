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
  TypeChecker tc;

  Module *root_module;
} Compiler;

void compiler_init(Compiler *c, Allocator *al);

void compiler_destroy(Compiler *c, Allocator *al);

// run the full compilation pipelineo
// only single file for now
void compiler_run(Compiler *c, const char *path);