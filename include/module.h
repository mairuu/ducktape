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

// a module name bound as a path qualifier by `use a::b;` (milestone 33). Unlike
// an item import it names no definition — it opens a namespace: `b::thing`
// resolves `thing` against b's `pub` exports. Stored on the importer; consumed
// only by path resolution.
typedef struct {
  StringView name; // the qualifier (the module's leaf, or an `as` alias)
  Module *target;  // the module it opens
  Span span;       // where it was bound, for a collision diagnostic
  bool *origin;    // the import's `used` flag, for the unused-import warning
} QualModule;

// an enum variant bound as a bare name by `use a::E::V;`. It is neither a type
// nor a value — a variant is only ever the head of a constructor or a pattern —
// so it gets a namespace of its own beside the qualifier list, checked for
// collisions against both scopes so the three cannot disagree about a name.
typedef struct {
  StringView name; // the bare name (the variant's, or an `as` alias)
  EnumDef *enum_def;
  // NULL while the entry is a *reservation*: a scope import claims its name in
  // source order, with the other imports, but cannot say what it names until
  // the module has resolved. Every reader outside linking runs a phase later,
  // by which time it is filled or the entry is dead.
  VariantDef *variant;
  bool is_pub;    // the `use` was a `pub use`: re-exportable like any item
  bool from_glob; // bound by `use E::*;`, so anything written by name wins
  // bound by the prelude, which nobody wrote: weaker still, and the one thing
  // a glob may take a name from. (An explicit import cannot collide with one —
  // the prelude's entries are appended last, so they link last and yield.)
  bool from_prelude;
  Span span;    // where it was bound, for a collision diagnostic
  bool *origin; // the import's `used` flag, for the unused-import warning
} VariantImport;

struct Module {
  // path as given on the command line, or derived from it; used verbatim for
  // fopen, as the diagnostic label, and as the module's registry key.
  // null-terminated.
  StringView file_path;

  // A second registry key this module also answers to. Only the root ever has
  // one, and only when it names a standard library source file: `std/cmp.dt`
  // on the command line and `use std::cmp` are then the *same module* rather
  // than two copies of one file. Empty otherwise. See `mod_adopt_std`.
  StringView alias_path;

  // The std leaf name if this is a standard library module, empty if not —
  // the `@lang` right, the prelude exemption, and the warning audience.
  // Separate from `file_path` because an adopted root is a std module whose
  // path is a real one; separate from where the source comes from, which
  // `mod_parse` still reads off `file_path`.
  StringView std_name;

  String source;
  Program *ast;

  ModImport *imports;
  int import_count;

  // module qualifiers this module's `use a::b;` declarations bind. Filled by
  // tc_link_imports, read by resolve_path when a path's first segment is one.
  QualModule *qual_modules;
  int qual_module_count, qual_module_cap;

  // variants this module's `use a::E::V;` declarations bind bare. Filled by
  // tc_link_imports, read wherever a lone name may name a variant: path
  // resolution, a path expression, and a binding pattern.
  VariantImport *variant_imports;
  int variant_import_count, variant_import_cap;

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

// the variant `m` bound bare under `name` (`use a::E::V;` → `V`), or NULL.
VariantImport *mod_find_variant_import(Module *m, StringView name);

// the same lookup without the "is it filled in" filter: the entry holding this
// name whatever state it is in. For linking, which owns those states.
VariantImport *mod_variant_import_slot(Module *m, StringView name);

Module *mod_new(StringView file_path, Allocator *al);

// The std leaf name an *entry path* names, or an empty view. A standard
// library file is `<dir>/std/<leaf>.dt` for a `<leaf>` the binary embeds; the
// parent component has to be `std` so that only a path spelling the library
// reaches the library. Lexical, like every other path question here.
StringView std_name_for_entry(StringView path);

// Make `m` the std module `name` while it keeps its own path. Naming a std
// source file on the command line must compile *that file* as the library
// module it is, or the prelude mints a second copy of it from the embedded
// table and every nominal type in it exists twice. The real path stays the
// diagnostic label — a lint has to point at a file a reader can open.
void mod_adopt_std(Module *m, StringView name, Allocator *al);

void mod_free(Module **m, Allocator *al);

bool mod_parse(Module *m, DiagBag *diags, Allocator *al);

// scan m->ast for DECL_USE, resolve each to a dependency file, and fill
// m->imports. files not yet seen are registered with `reg` as unparsed stubs,
// which is what drives discovery. emits one diagnostic per unresolvable
// import; returns false if any failed. requires m to be parsed.
bool mod_collect_imports(Module *m, ModuleRegistry *reg, StringView base_dir,
                         DiagBag *diags, Allocator *al);

// append the implicit prelude to a non-std module: synthesised imports of the
// vocabulary and lang-item std modules, so their names and lang items are in
// scope without an explicit `use`. A no-op for std modules (which cannot
// import themselves). Registers any prelude module not yet seen, extending the
// discovery worklist the same way mod_collect_imports does. Requires m parsed.
void mod_inject_prelude(Module *m, ModuleRegistry *reg, Allocator *al);

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

// is this any embedded std module? The gate for `@lang`.
bool mod_is_std_module(const Module *m);

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