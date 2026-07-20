#pragma once

#include "allocator.h"
#include "ast.h"
#include "sema.h"

typedef struct Module Module;
typedef struct ModuleRegistry ModuleRegistry;

// ═══════════════════════════════════════════════════════════════════════════════
// Module
// ═══════════════════════════════════════════════════════════════════════════════

struct Module {
  StringView file_path; // absolute filesystem path, null-terminated
  String source;
  Program *ast;

  FunDef **funs;
  int fun_count, fun_cap;

  // impl methods, flattened; populated by codegen_module. Slots for
  // OP_GET_GLOBAL continue past `funs` (fun_count..fun_count+method_count-1)
  // so both share one callable-value slot space.
  FunDef **methods;
  int method_count;

  StructDef **structs;
  int struct_count, struct_cap;

  EnumDef **enums;
  int enum_count, enum_cap;

  ImplDef **impls;
  int impl_count, impl_cap;

  ValueScope vscope; // module-level values: functions, global vars
  TypeScope tscope;  // module-level types: structs, enums, traits
};

Module *mod_new(StringView file_path, Allocator *al);

void mod_free(Module **m, Allocator *al);

bool mod_parse(Module *m, DiagBag *diags, Allocator *al);

// ═══════════════════════════════════════════════════════════════════════════════
// ModuleRegistry
// ═══════════════════════════════════════════════════════════════════════════════

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