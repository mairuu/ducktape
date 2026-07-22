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
// macros — and an implicit declaration is a hard error in C23. A module path
// is a fixed base dir plus identifier segments, which can never contain '.' or
// '..', so lexical normalisation makes two spellings of one file
// byte-identical. It also works for files that don't exist, which is what lets
// a missing-module diagnostic name the path it looked for.
// The "."/".." folding is not dead: the root path comes from argv and may
// contain either. Not deduped: symlinks, and paths differing before the base
// dir.

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
  StringView file_path; // dependency file (a `<std>/…` key for std modules)
  int module_index;     // index into ModuleRegistry; -1 if unresolved
} ModImport;

struct Module {
  // path as given on the command line, or derived from it; used verbatim for
  // fopen and as the registry key. null-terminated.
  StringView file_path;
  String source;
  Program *ast;

  ModImport *imports;
  int import_count;

  FunDef **funs;
  int fun_count, fun_cap;

  StructDef **structs;
  int struct_count, struct_cap;

  EnumDef **enums;
  int enum_count, enum_cap;

  ImplDef **impls;
  int impl_count, impl_cap;

  // every impl this module may select from: its own, plus those of every
  // module reachable through `use`, transitively. An impl is not an item with
  // a name, so it cannot be imported one at a time — reachability is the only
  // handle a module has on it, and it must be transitive because `pub use`
  // re-exports a type whose impls live one module further away.
  //
  // A *union* rather than a filter over one global list, so every
  // impl_index_* lookup keeps the signature it had and only its argument
  // changes. Built in topological order by tc_import_impls, then extended
  // with the module's own as resolution reaches each `impl` block.
  ImplIndex visible_impls;

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

// is this the embedded std module of that name (`mod_is_std(m, "fmt")`)?
bool mod_is_std(const Module *m, const char *name);

void modreg_init(ModuleRegistry *reg, Allocator *al);

void modreg_destroy(ModuleRegistry *reg);

// add a module, returning its index. if one with the same path is already
// registered, nothing is inserted and *that* module's index comes back — the
// dedup that makes a diamond import one Module.
int modreg_add(ModuleRegistry *reg, Module *m);

// index of the module with this exact (normalised) path, or -1.
int modreg_find(ModuleRegistry *reg, StringView path);

static inline Module *modreg_topo(ModuleRegistry *reg, int idx) {
  return reg->modules[reg->topo_order[idx]];
}