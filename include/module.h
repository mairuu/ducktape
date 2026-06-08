#pragma once

#include "allocator.h"
#include "ast.h"
#include "sema.h"

typedef struct Module Module;
typedef struct ModuleRegistry ModuleRegistry;

struct Module {
  StringView file_path; // absolute filesystem path, null-terminated
  String source;
  Program *ast;

  FunDef **funs;
  int fun_count, fun_cap;

  ValueScope vscope; // module-level values: functions, global vars
};

Module *mod_new(StringView file_path, Allocator *al);

void mod_free(Module **m, Allocator *al);

bool mod_parse(Module *m, DiagBag *diags, Allocator *al);

struct ModuleRegistry {
  Module **modules;
  int module_count;
  int module_cap;

  int *topo_order; // indices into modules, in topological order
  int topo_count;

  Allocator *al;
};

void modreg_init(ModuleRegistry *reg, Allocator *al);

void modreg_destroy(ModuleRegistry *reg);

// add a module. returns false if a module with the same path already exists.
bool modreg_add(ModuleRegistry *reg, Module *m);

static inline Module *modreg_topo(ModuleRegistry *reg, int idx) {
  return reg->modules[reg->topo_order[idx]];
}