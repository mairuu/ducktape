#pragma once

#include "allocator.h"
#include "ast.h"
#include "sema.h"

typedef struct Module Module;
typedef struct ModuleRegistry ModuleRegistry;

// ═══════════════════════════════════════════════════════════════════════════════
// Paths
// ═══════════════════════════════════════════════════════════════════════════════

// Path handling is deliberately lexical: the build is -std=c23, which defines
// __STRICT_ANSI__ and so hides realpath/getcwd/PATH_MAX behind glibc's feature
// macros — and an implicit declaration is a hard error in C23. Every module
// path is a fixed base dir plus identifier segments, which can never contain
// '.' or '..', so lexical normalisation makes two spellings of one file
// byte-identical. It also works for files that don't exist, which is what lets
// a missing-module diagnostic name the path it looked for.
// Not deduped: symlinks, and paths differing before the base dir.

// everything up to and including the last '/'; empty when there is none.
StringView path_dir_of(StringView path);

// collapse "//", drop "./" components, fold "x/../" pairs.
// returns a null-terminated copy allocated from `al`.
StringView path_normalise(StringView path, Allocator *al);

// "<base_dir>" + `path`'s segments joined by '/' + ".dt", normalised.
// `path` is a use declaration's module prefix (DeclUse.path).
StringView mod_file_for_use(StringView base_dir, const Path *path,
                            Allocator *al);

// ═══════════════════════════════════════════════════════════════════════════════
// Module
// ═══════════════════════════════════════════════════════════════════════════════

// one resolved `use` declaration.
typedef struct {
  Decl *decl;           // the DECL_USE node (path + aliases + spans)
  StringView file_path; // dependency file; empty when is_std
  bool is_std;          // `use std::..` — the builtins, never the filesystem
  int module_index;     // index into ModuleRegistry; -1 for std / unresolved
} ModImport;

struct Module {
  // path as given on the command line, or derived from it; used verbatim for
  // fopen and as the registry key. null-terminated.
  StringView file_path;
  String source;
  Program *ast;

  ModImport *imports;
  int import_count, import_cap;

  FunDef **funs;
  int fun_count, fun_cap;

  // impl methods, flattened; populated by codegen_module. Slots for
  // OP_GET_GLOBAL continue past `funs` (fun_count..fun_count+method_count-1)
  // so both share one callable-value slot space.
  FunDef **methods;
  int method_count;

  // nested closure functions, collected by codegen. not addressable by
  // OP_GET_GLOBAL (they're built at runtime via OP_CLOSURE), but their chunks'
  // constant pools still need to be GC roots like every other compiled chunk.
  FunDef **closures;
  int closure_count, closure_cap;

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

// scan m->ast for DECL_USE, resolve each to a dependency file, and fill
// m->imports. files not yet seen are registered with `reg` as unparsed stubs,
// which is what drives discovery. emits one diagnostic per unresolvable
// import; returns false if any failed. requires m to be parsed.
bool mod_collect_imports(Module *m, ModuleRegistry *reg, StringView base_dir,
                         DiagBag *diags, Allocator *al);

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

// add a module. returns false without inserting if one with the same path is
// already registered — that dedup is what makes a diamond import one Module.
bool modreg_add(ModuleRegistry *reg, Module *m);

// exact (normalised) path match; null when absent.
Module *modreg_find(ModuleRegistry *reg, StringView path);

// index of `m` in `reg`, or -1.
int modreg_index_of(ModuleRegistry *reg, Module *m);

static inline Module *modreg_topo(ModuleRegistry *reg, int idx) {
  return reg->modules[reg->topo_order[idx]];
}