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

// ═══════════════════════════════════════════════════════════════════════════════
// Module
// ═══════════════════════════════════════════════════════════════════════════════

// one resolved `use` declaration.
typedef struct {
  Decl *decl;           // the DECL_USE node (path + aliases + spans)
  StringView file_path; // the dependency's key (a `<std>/…` one for a library
                        // module); empty when the path named no module
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

// a module globbed by `use a::*;`. Every other import kind writes an alias, so
// its source can be recovered from the declaration; a glob writes none, and the
// export walk needs the module because a `pub use a::*;` re-exports whatever
// `a` exports.
typedef struct {
  Module *source;
  Decl *decl; // the `use` — its span, and whether it was a `pub use`
} GlobImport;

// one `mod x;` declaration, resolved to the module it names.
typedef struct {
  StringView name;
  Module *mod;
  Decl *decl; // the DECL_MOD node — the span a privacy diagnostic points at
} ModChild;

struct Module {
  // the module's source file: fopen argument, diagnostic header, registry key.
  // A library module's is the synthetic `<std>/…` key its embedded source has
  // no real path for. null-terminated.
  StringView file_path;

  // the module as a `use` path spells it — `std::collections::hashmap`,
  // `geo::shape`. Empty for the program root, which nothing names. This is
  // what a diagnostic about *a module* prints; `file_path` is what one printed
  // *against* a module points at.
  StringView label;

  // ── the tree ───────────────────────────────────────────────────────────────
  Module *parent;  // NULL for a root
  StringView name; // this module's own segment; empty for a root
  bool is_std;     // in the library tree: the `@lang` right, no prelude, and
                   // no warning audience unless it is the root being linted
  bool is_pub;     // declared `pub mod`; a root is public by fiat
  bool from_disk;  // a library module read from the filesystem instead of the
                   // embedded table: the `--std-module` lint target
  int reg_index;   // index in the ModuleRegistry, or -1 while unregistered

  // what a child's own name is appended to. For a program module that is a
  // directory (`tests/x/geo/`); for a library module it is the library-relative
  // prefix the embedded table is keyed by (`collections/`). Either way
  // source(child) = child_prefix + child->name, which is the whole of §2.2.
  StringView child_prefix;
  StringView
      std_rel; // library-relative name, no extension (`collections/hashmap`)

  ModChild *children;
  int child_count, child_cap;

  String source;
  Program *ast;
  bool load_failed; // parsed once and failed; do not try again

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

  // modules this one globs (`use a::*;`). Filled by tc_link_imports, read by
  // the export walk — a glob is the only import whose names are not written
  // down anywhere, so this list is the record that it happened.
  GlobImport *glob_imports;
  int glob_import_count, glob_import_cap;

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

// the program root, whose source is the entry path and whose children live
// beside it. Registered by the caller.
Module *mod_new_program_root(StringView entry_path, Allocator *al);

// the library root: `std/lib.dt`, the one statement of what `std::` names.
Module *mod_new_std_root(Allocator *al);

// read this library module from the filesystem (`std/<rel>.dt`) instead of the
// embedded table — the `--std-module` lint hatch, so a lint sees the edit and
// not whatever `make` last mirrored. Its dependencies stay embedded. Must be
// called before the module is parsed.
void mod_read_from_disk(Module *m, Allocator *al);

// the child `m` declares under `name`, or NULL. Requires m parsed.
ModChild *mod_find_child(Module *m, StringView name);

void mod_free(Module **m, Allocator *al);

bool mod_parse(Module *m, DiagBag *diags, Allocator *al);

// build the child modules `m`'s `mod` declarations name (§2.1–2.2). A program
// child is registered here and so is compiled whether or not anything imports
// it — the program's tree *is* its build unit. A library child is linked into
// the tree and left unregistered: std is a dependency, and a dependency is
// loaded where it is used. Reports a `mod` naming a source that does not
// exist, once, against the declaration. Requires m parsed.
bool mod_declare_children(Module *m, ModuleRegistry *reg, DiagBag *diags,
                          Allocator *al);

// register every module `m`'s use paths walk through, so the discovery loop
// parses them and the tree keeps growing. Deliberately silent: a path that
// names nothing is diagnosed by mod_collect_imports, once the tree has stopped
// growing and the answer is final. Requires m parsed.
void mod_reach_imports(Module *m, ModuleRegistry *reg);

// resolve every `use` in m->ast against the finished tree (§3) and fill
// m->imports. Emits one diagnostic per unresolvable import; returns false if
// any failed. Requires m parsed and the tree built.
bool mod_collect_imports(Module *m, ModuleRegistry *reg, DiagBag *diags,
                         Allocator *al);

// append the implicit prelude to a non-std module: synthesised imports of the
// vocabulary and lang-item std modules, so their names and lang items are in
// scope without an explicit `use`. A no-op for std modules (which cannot
// import themselves). Registers any prelude module not yet seen, extending the
// discovery worklist the same way mod_reach_imports does. Requires m parsed
// and `reg->std_root` parsed.
void mod_inject_prelude(Module *m, ModuleRegistry *reg, Allocator *al);

// ═══════════════════════════════════════════════════════════════════════════════
// ModuleRegistry
// ═══════════════════════════════════════════════════════════════════════════════

struct ModuleRegistry {
  Module **modules;
  int module_count;
  int module_cap;

  // the two roots (§2). `program_root` is NULL when the compile has no program
  // — a `--std-module` lint, whose only tree is the library's.
  Module *program_root;
  Module *std_root;

  int *topo_order; // indices into modules, in topological order
  int topo_count;

  Allocator *al;
};

// is this any standard library module? The gate for `@lang`.
static inline bool mod_is_std_module(const Module *m) { return m->is_std; }

// how to name this module *in* a diagnostic: the path a reader would write,
// falling back to the file for the program root, which no path can name.
static inline StringView mod_label(const Module *m) {
  return m->label.len > 0 ? m->label : m->file_path;
}

// split `std::a::b` into its segments, returning the count. The one place a
// module path arrives from outside a source file: the `--std-module` lint
// hatch, whose descent has to load each module to see the next one's name.
int mod_split_path(const char *path, StringView *out, int max);

void modreg_init(ModuleRegistry *reg, Allocator *al);

void modreg_destroy(ModuleRegistry *reg);

// add a module to the build, returning its index; a no-op for one already in.
// Registration is what makes a module *compiled*: the tree can hold a library
// module nothing has imported, and that module is never registered.
int modreg_add(ModuleRegistry *reg, Module *m);

static inline Module *modreg_topo(ModuleRegistry *reg, int idx) {
  return reg->modules[reg->topo_order[idx]];
}