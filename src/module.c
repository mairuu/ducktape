#include "module.h"
#include "allocator.h"
#include "diag.h"
#include "parser.h"
#include "scanner.h"
#include "sema.h"
#include "std_src.h"
#include "string_utils.h"

#include <assert.h>
#include <dirent.h>
#include <stdio.h>
#include <string.h>

StringView path_dir_of(StringView path) {
  for (int i = path.len - 1; i >= 0; i--) {
    if (path.chars[i] == '/') {
      return (StringView){.chars = path.chars, .len = i + 1};
    }
  }
  return (StringView){.chars = path.chars, .len = 0};
}

StringView path_normalise(StringView path, Allocator *al) {
  // worst case the output is the input; +1 for the terminator.
  char *out = al_alloc(al, sizeof(char) * (size_t)(path.len + 1));
  int n = 0;

  // byte offsets in `out` where each kept component starts, so ".." can pop
  // one. a leading "/" is not a component and is never popped.
  int *starts = al_alloc(al, sizeof(int) * (size_t)(path.len + 1));
  int depth = 0;

  int i = 0;
  if (path.len > 0 && path.chars[0] == '/') {
    out[n++] = '/';
    i = 1;
  }

  while (i < path.len) {
    if (path.chars[i] == '/') { // collapse runs of '/'
      i++;
      continue;
    }

    int start = i;
    while (i < path.len && path.chars[i] != '/') {
      i++;
    }
    int len = i - start;

    if (len == 1 && path.chars[start] == '.') {
      continue; // drop "./"
    }

    if (len == 2 && path.chars[start] == '.' && path.chars[start + 1] == '.' &&
        depth > 0) {
      n = starts[--depth]; // fold "x/.." — rewind over "x/"
      continue;
    }

    starts[depth++] = n;
    memcpy(out + n, path.chars + start, (size_t)len);
    n += len;
    if (i < path.len) {
      out[n++] = '/';
    }
  }

  al_free(al, starts, sizeof(int) * (size_t)(path.len + 1));
  out[n] = '\0';
  return (StringView){.chars = out, .len = n};
}

// ═══════════════════════════════════════════════════════════════════════════════
// The module tree
// ═══════════════════════════════════════════════════════════════════════════════
//
// There are exactly two trees (`references/architecture.md`, "Modules"): the
// program's, rooted at the entry file, and the library's, rooted at
// `std/lib.dt` and reached through the reserved first segment `std`. Every
// module below a root is there because some module *declared* it with `mod`,
// so a path is walked, never guessed — nothing here asks whether a file
// exists.
//
// The two trees differ in one respect, and it is the perf-shaped one: the
// program's tree is its build unit and is registered whole, so a module
// nothing imports is still compiled; the library's is a catalogue, and a
// library module is registered when a path reaches it. Registering all of std
// eagerly would compile 20 modules where the prelude closure needs 11, and the
// prelude is already most of a trivial compile.

// The registry key (and diagnostic header) of an embedded library module. The
// angle brackets are what keep it distinct from every user path: a real one is
// a base dir plus identifier segments, and an identifier cannot contain '<'.
// `rel` may nest (`collections/hashmap`) and nothing here has to know — it is
// already the library-relative path the embedded table is keyed by.
static StringView std_mod_key(StringView rel, Allocator *al) {
  const char *prefix = "<std>/";
  int prefix_len = 6;
  int len = prefix_len + rel.len + 3; // ".dt"

  char *buf = al_alloc(al, sizeof(char) * (size_t)(len + 1));
  memcpy(buf, prefix, (size_t)prefix_len);
  memcpy(buf + prefix_len, rel.chars, (size_t)rel.len);
  memcpy(buf + prefix_len + rel.len, ".dt", 3);
  buf[len] = '\0';

  return (StringView){.chars = buf, .len = len};
}

// concatenate, null-terminated: the one shape every path here is built in.
// An empty view may carry a NULL `chars` — the program root's label is exactly
// that — and memcpy from NULL is undefined even for a zero count, so each part
// is copied only when it has bytes.
static StringView sv_concat3(StringView a, StringView b, const char *c,
                             Allocator *al) {
  int c_len = (int)strlen(c);
  int len = a.len + b.len + c_len;
  char *buf = al_alloc(al, sizeof(char) * (size_t)(len + 1));
  if (a.len > 0) {
    memcpy(buf, a.chars, (size_t)a.len);
  }
  if (b.len > 0) {
    memcpy(buf + a.len, b.chars, (size_t)b.len);
  }
  memcpy(buf + a.len + b.len, c, (size_t)c_len);
  buf[len] = '\0';
  return (StringView){.chars = buf, .len = len};
}

VariantImport *mod_find_variant_import(Module *m, StringView name) {
  VariantImport *slot = mod_variant_import_slot(m, name);
  // a reservation names nothing yet, and a dead one never will: both read as
  // "no such variant" everywhere outside the linking that owns them.
  if (slot == NULL || slot->variant == NULL) {
    return NULL;
  }
  // this is the *use* lookup — the slot form beside it is the linker's, which
  // asks whether a name is taken and so marks nothing.
  if (slot->origin != NULL) {
    *slot->origin = true;
  }
  return slot;
}

VariantImport *mod_variant_import_slot(Module *m, StringView name) {
  if (m == NULL) {
    return NULL;
  }
  for (int i = 0; i < m->variant_import_count; i++) {
    if (sv_equal(m->variant_imports[i].name, name)) {
      return &m->variant_imports[i];
    }
  }
  return NULL;
}

static Module *mod_new(StringView file_path, Allocator *al) {
  Module *m = al_alloc_zero_for(al, Module);
  m->file_path = file_path;
  m->reg_index = -1;
  m->is_pub = true;
  vscope_init(&m->vscope, NULL, al);
  tscope_init(&m->tscope, NULL, al);
  impl_index_init(&m->visible_impls, al);
  return m;
}

Module *mod_new_program_root(StringView entry_path, Allocator *al) {
  Module *m = mod_new(entry_path, al);
  // a root's children live in its own directory, which is why every existing
  // layout already matched this rule and the migration moved no file.
  m->child_prefix = path_dir_of(entry_path);
  return m;
}

Module *mod_new_std_root(Allocator *al) {
  StringView rel = sv_from_cstr("lib");
  Module *m = mod_new(std_mod_key(rel, al), al);
  m->is_std = true;
  m->std_rel = rel;
  m->label = sv_from_cstr("std");
  m->child_prefix = (StringView){.chars = "", .len = 0};
  return m;
}

void mod_read_from_disk(Module *m, Allocator *al) {
  assert(m->is_std);
  m->from_disk = true;
  // the real path becomes the diagnostic header too: a lint has to point at a
  // file its reader can open.
  m->file_path = sv_concat3(sv_from_cstr("std/"), m->std_rel, ".dt", al);
}

ModChild *mod_find_child(Module *m, StringView name) {
  for (int i = 0; i < m->child_count; i++) {
    if (sv_equal(m->children[i].name, name)) {
      return &m->children[i];
    }
  }
  return NULL;
}

void mod_free(Module **m, Allocator *al) {
  assert(m != NULL);
  al_free_for(al, *m);
}

static String read_file(const char *path, Allocator *al);

static bool file_exists(const char *path) {
  FILE *f = fopen(path, "rb");
  if (f == NULL) {
    return false;
  }
  fclose(f);
  return true;
}

// The layout rule in full: a child's source is its parent's child directory
// plus its own name, and its child directory is that source without the
// extension. For a library module the same two lines run over library-relative
// names, because that is the shape the embedded table is keyed by.
static Module *mod_new_child(Module *parent, Decl *decl, Allocator *al) {
  StringView name = decl->as.mod_decl.name;
  StringView slash = sv_from_cstr("/");

  Module *m;
  if (parent->is_std) {
    StringView rel = sv_concat3(parent->child_prefix, name, "", al);
    m = mod_new(std_mod_key(rel, al), al);
    m->std_rel = rel;
    m->child_prefix = sv_concat3(rel, slash, "", al);
  } else {
    m = mod_new(sv_concat3(parent->child_prefix, name, ".dt", al), al);
    m->child_prefix = sv_concat3(parent->child_prefix, name, "/", al);
  }

  m->parent = parent;
  m->name = name;
  m->is_std = parent->is_std;
  m->is_pub = decl->is_pub;
  m->label = parent->label.len > 0
                 ? sv_concat3(parent->label, sv_from_cstr("::"), "", al)
                 : (StringView){0};
  m->label = sv_concat3(m->label, name, "", al);
  return m;
}

static void mod_push_child(Module *parent, StringView name, Module *child,
                           Decl *decl, Allocator *al) {
  if (parent->child_count == parent->child_cap) {
    int cap = parent->child_cap ? parent->child_cap * 2 : 4;
    parent->children = al_realloc(al, parent->children,
                                  sizeof(ModChild) * (size_t)parent->child_cap,
                                  sizeof(ModChild) * (size_t)cap);
    parent->child_cap = cap;
  }
  parent->children[parent->child_count++] =
      (ModChild){.name = name, .mod = child, .decl = decl};
}

// the orphan scan's working list, kept sorted: `readdir` has no order, and a
// diagnostic whose order changes between runs is a test that fails at random.
typedef struct {
  StringView *names;
  int count, cap;
} OrphanList;

static bool sv_less(StringView a, StringView b) {
  int n = a.len < b.len ? a.len : b.len;
  int c = n > 0 ? memcmp(a.chars, b.chars, (size_t)n) : 0;
  return c != 0 ? c < 0 : a.len < b.len;
}

static void orphan_push(OrphanList *l, StringView name, Allocator *al) {
  if (l->count == l->cap) {
    int cap = l->cap ? l->cap * 2 : 4;
    l->names = al_realloc(al, l->names, sizeof(StringView) * (size_t)l->cap,
                          sizeof(StringView) * (size_t)cap);
    l->cap = cap;
  }
  int i = l->count++;
  for (; i > 0 && sv_less(name, l->names[i - 1]); i--) {
    l->names[i] = l->names[i - 1];
  }
  l->names[i] = name;
}

// `orphan_module`: a `.dt` file in the directory a module owns that no `mod`
// of that module claims — a file outside the build that does not say so.
//
// A *program* root is exempt, and that is the one asymmetry: several roots
// share one directory (every single-file test in `tests/run/` is a sibling of
// every other), so a base directory is owned by nobody. The library root does
// own `std/`, so "a new std file needs a `pub mod`" stops being a silence.
static void mod_warn_orphans(Module *m, DiagBag *diags, Allocator *al) {
  if (m->parent == NULL && !m->is_std) {
    return;
  }

  OrphanList orphans = {0};
  if (m->is_std) {
    for (int i = 0;; i++) {
      const char *entry = std_module_at(i);
      if (entry == NULL) {
        break;
      }
      if (strncmp(entry, m->child_prefix.chars, (size_t)m->child_prefix.len) !=
          0) {
        continue;
      }
      const char *leaf = entry + m->child_prefix.len;
      // a grandchild is some other module's directory to answer for, and the
      // root's own `lib` is its source rather than a child of itself.
      if (strchr(leaf, '/') != NULL || sv_equal_cstr(m->std_rel, entry)) {
        continue;
      }
      StringView name = sv_from_cstr(leaf);
      if (mod_find_child(m, name) == NULL) {
        orphan_push(&orphans, name, al);
      }
    }
  } else {
    DIR *dir = opendir(m->child_prefix.chars);
    if (dir == NULL) {
      return; // a module with no children need not have a directory
    }
    for (struct dirent *e; (e = readdir(dir)) != NULL;) {
      size_t len = strlen(e->d_name);
      if (len <= 3 || strcmp(e->d_name + len - 3, ".dt") != 0) {
        continue;
      }
      StringView name = {.chars = e->d_name, .len = (int)len - 3};
      if (mod_find_child(m, name) != NULL) {
        continue;
      }
      // `d_name` is reused by the next readdir, so the arena keeps the copy.
      orphan_push(&orphans, sv_concat3(name, (StringView){0}, "", al), al);
    }
    closedir(dir);
  }
  if (orphans.count == 0) {
    return;
  }

  // the allow lives on the declaration that named this module — one file up,
  // where the decision to have the directory was made. A root has no such
  // declaration and so cannot be silenced.
  ModChild *self =
      m->parent != NULL ? mod_find_child(m->parent, m->name) : NULL;
  unsigned saved =
      diag_push_allowed(diags, self != NULL ? self->decl->allow_mask : 0);

  // anchored on the `mod` list the file should have joined; a module with no
  // list yet gets its first line, which is where one would start.
  Span span = {.line = 1, .col = 1, .line_end = 1, .col_end = 2};
  for (int i = 0; i < m->ast->decl_count; i++) {
    if (m->ast->decls[i]->kind == DECL_MOD) {
      span = m->ast->decls[i]->span;
    }
  }

  for (int i = 0; i < orphans.count; i++) {
    StringView rel = sv_concat3(m->child_prefix, orphans.names[i], ".dt", al);
    StringView file =
        m->is_std ? sv_concat3(sv_from_cstr("std/"), rel, "", al) : rel;
    // one line, no note: a note is not gated by the allow mask, so advice that
    // belongs to a silenceable warning has to travel inside it.
    diag_warning(diags, LINT_ORPHAN_MODULE, span,
                 "'" SV_FMT "' is not a module: no `mod " SV_FMT
                 ";` declares it",
                 SV_ARG(file), SV_ARG(orphans.names[i]));
  }
  diag_pop_allowed(diags, saved);
}

bool mod_declare_children(Module *m, ModuleRegistry *reg, DiagBag *diags,
                          Allocator *al) {
  assert(m->ast != NULL);

  bool ok = true;
  for (int i = 0; i < m->ast->decl_count; i++) {
    Decl *decl = m->ast->decls[i];
    if (decl->kind != DECL_MOD) {
      continue;
    }
    StringView name = decl->as.mod_decl.name;

    // `std` is a reserved first segment: the library is addressed through
    // it from every module, so a program cannot spend it on a child.
    if (m == reg->program_root && sv_equal_cstr(name, "std")) {
      diag_error(diags, decl->as.mod_decl.name_span,
                 "'std' is reserved for the standard library and cannot name a "
                 "module of this program");
      ok = false;
      continue;
    }
    // two `mod` declarations of one name are the ordinary duplicate-name error,
    // reported by the checker over every declaration kind at once.
    if (mod_find_child(m, name) != NULL) {
      continue;
    }

    Module *child = mod_new_child(m, decl, al);
    bool exists = m->is_std ? std_module_source(child->std_rel) != NULL
                            : file_exists(child->file_path.chars);
    if (!exists) {
      // once, here, rather than once per importer: a declared module with no
      // source is a fact about the tree, not about anyone's use of it.
      diag_error(diags, decl->as.mod_decl.name_span,
                 "cannot find module '" SV_FMT "'", SV_ARG(name));
      diag_note(diags, (Span){0}, "expected file '" SV_FMT "'",
                SV_ARG(m->is_std ? child->std_rel : child->file_path));
      ok = false;
    }
    // the child goes into the tree either way, so a path through it stops at a
    // module that is merely unloaded — and every `use` naming it stays quiet.
    mod_push_child(m, name, child, decl, al);

    // the program's tree is its build unit: a module nothing imports is still
    // compiled, which is the whole point of declaring it.
    if (exists && !m->is_std) {
      modreg_add(reg, child);
    }
  }
  mod_warn_orphans(m, diags, al);
  return ok;
}

// Anchor the path at the root its first segment names, then
// descend while a segment names a declared child. `*consumed` comes back as the
// number of segments eaten, counting a leading `std`. The walk is greedy and
// cannot be ambiguous, because a name may not be both a declared child and
// an item.
//
// `reach` registers each module descended into — which is how a library module
// joins the build, and why discovery iterates to a fixpoint: a child's own
// children are only known once it has been parsed.
static Module *mod_walk(ModuleRegistry *reg, Module *from, const Path *path,
                        int *consumed, bool reach) {
  bool std_rooted =
      path->count > 0 && sv_equal_cstr(path->segments[0].name, "std");

  *consumed = 0;
  // A library module is anchored at the library root and nowhere else: `std`
  // is where its own tree begins, and it must not be able to name a file of
  // the program that happens to be compiling it.
  if (!std_rooted && (from->is_std || reg->program_root == NULL)) {
    return NULL;
  }

  Module *cur = std_rooted ? reg->std_root : reg->program_root;
  int i = std_rooted ? 1 : 0;
  *consumed = i;

  while (cur->ast != NULL && i < path->count) {
    ModChild *ch = mod_find_child(cur, path->segments[i].name);
    if (ch == NULL) {
      break;
    }
    cur = ch->mod;
    *consumed = ++i;
    if (reach) {
      modreg_add(reg, cur);
    }
  }
  return cur;
}

// how a diagnostic names a module's source. A library module's registry key is
// the synthetic `<std>/…` its embedded text has no real path for, and a reader
// told to open that has been told nothing.
static StringView mod_display_path(const Module *m, Allocator *al) {
  return m->is_std && !m->from_disk
             ? sv_concat3(sv_from_cstr("std/"), m->std_rel, ".dt", al)
             : m->file_path;
}

// A module is reachable from `from` when every component of its path is
// `pub` or was declared inside `from`'s own subtree. Returns the component
// nearest the root that is neither — the first door the path could not have
// opened — or NULL when the whole path is reachable.
//
// The walk itself stays permissive: privacy is a fact about a `use`, not about
// the tree, so it is checked once where a path is read and never during
// discovery, which must reach a module in order to report anything about it.
static Module *mod_first_private(Module *from, Module *target) {
  Module *offender = NULL;
  for (Module *c = target; c->parent != NULL; c = c->parent) {
    if (c->is_pub) {
      continue;
    }
    bool inside = false;
    for (Module *a = from; a != NULL && !inside; a = a->parent) {
      inside = a == c->parent;
    }
    if (!inside) {
      offender = c;
    }
  }
  return offender;
}

// register the prelude's library modules. Declared here because the prelude
// table is, but reached from mod_reach_imports: the prelude is an import
// nobody wrote, and discovery must find it the same round it finds the others.
static void reach_prelude(ModuleRegistry *reg);

void mod_reach_imports(Module *m, ModuleRegistry *reg) {
  assert(m->ast != NULL);
  if (!m->is_std) {
    reach_prelude(reg);
  }
  for (int i = 0; i < m->ast->decl_count; i++) {
    Decl *decl = m->ast->decls[i];
    if (decl->kind != DECL_USE) {
      continue;
    }
    int consumed;
    (void)mod_walk(reg, m, &decl->as.use_decl.path, &consumed, /*reach=*/true);
  }
}

// render a use path back as `a::b` for diagnostics. exact-sized, so there is
// no bound to juggle and nothing truncates.
static StringView sprint_mod_path(const Path *path, int count, Allocator *al) {
  int len = 0;
  for (int i = 0; i < count; i++) {
    len += path->segments[i].name.len + (i > 0 ? 2 : 0);
  }

  char *buf = al_alloc(al, sizeof(char) * (size_t)(len + 1));
  int n = 0;
  for (int i = 0; i < count; i++) {
    if (i > 0) {
      buf[n++] = ':';
      buf[n++] = ':';
    }
    StringView seg = path->segments[i].name;
    memcpy(buf + n, seg.chars, (size_t)seg.len);
    n += seg.len;
  }
  buf[n] = '\0';
  return (StringView){.chars = buf, .len = n};
}

// the path named no module below its root. For the library that is the whole
// answer; for the program the head may still be an *enum* in this module's own
// scope — the module-less reading — so this reports only once that is out.
static void report_no_module(const Path *path, bool std_rooted, DiagBag *diags,
                             Allocator *al) {
  if (std_rooted) {
    // name what was looked for, not the whole path: with nothing resolved
    // there is no telling how many trailing segments were meant as the module,
    // and the first segment after `std` has to exist either way.
    int count = path->count > 2 ? 2 : path->count;
    diag_error(diags, path->span,
               "there is no standard library module '" SV_FMT "'",
               SV_ARG(sprint_mod_path(path, count, al)));
    diag_note(diags, (Span){0}, "available: %s", std_module_names());
    return;
  }
  StringView head = path->segments[0].name;
  diag_error(diags, path->span, "cannot find module '" SV_FMT "'",
             SV_ARG(head));
  diag_note(diags, (Span){0}, "no 'mod " SV_FMT ";' declares it", SV_ARG(head));
}

bool mod_collect_imports(Module *m, ModuleRegistry *reg, DiagBag *diags,
                         Allocator *al) {
  assert(m->ast != NULL);

  // exact-sized: nothing appends to m->imports after this except the prelude,
  // which reallocs.
  int use_count = 0;
  for (int i = 0; i < m->ast->decl_count; i++) {
    if (m->ast->decls[i]->kind == DECL_USE) {
      use_count++;
    }
  }
  if (use_count == 0) {
    return true;
  }
  m->imports = al_alloc_zero(al, sizeof(ModImport) * use_count);

  bool ok = true;
  for (int i = 0; i < m->ast->decl_count; i++) {
    Decl *decl = m->ast->decls[i];
    if (decl->kind != DECL_USE) {
      continue;
    }

    Path *path = &decl->as.use_decl.path;
    ModImport *imp = &m->imports[m->import_count++];
    imp->decl = decl;
    imp->module_index = -1;

    // `parse_path` in PATH_USE mode accepts turbofish segments; a module
    // prefix can't have type arguments.
    bool bad_args = false;
    for (int j = 0; j < path->count && !bad_args; j++) {
      bad_args = path->segments[j].type_arg_count != 0;
    }
    if (bad_args) {
      diag_error(diags, path->span,
                 "type arguments are not allowed in a use path");
      ok = false;
      continue;
    }

    bool bare = decl->as.use_decl.bare;
    bool glob = decl->as.use_decl.target.is_glob;
    bool std_rooted =
        path->count > 0 && sv_equal_cstr(path->segments[0].name, "std");

    int consumed;
    Module *cur = mod_walk(reg, m, path, &consumed, /*reach=*/false);
    int depth = consumed - (std_rooted ? 1 : 0);
    int rest = path->count - consumed;

    // A root is not addressable — the program's has no name and `std`
    // alone names the library, not a module of it — so a walk that stayed at
    // one found no module. The head may still be an enum already in scope,
    // which resolves a pass later (`tc_link_scope_imports`); a `std::` path
    // never gets that reading, since it consumed its root.
    if (depth == 0) {
      if (!std_rooted && rest == (bare ? 2 : 1)) {
        decl->as.use_decl.is_scope_import = true;
        decl->as.use_decl.qualifier = path->segments[0].name;
        continue;
      }
      report_no_module(path, std_rooted, diags, al);
      ok = false;
      continue;
    }

    // a declared module whose source never loaded: discovery reported it once,
    // at the `mod`, and every use of it stays quiet.
    if (cur->ast == NULL) {
      imp->file_path = cur->file_path;
      continue;
    }

    // Privacy is reported here rather than at the walk, so it survives the two
    // readings above: a path that named no module at all has a different
    // answer, and a module with no source has already been reported once.
    Module *hidden = mod_first_private(m, cur);
    if (hidden != NULL) {
      diag_error(diags, path->span, "module '" SV_FMT "' is private",
                 SV_ARG(hidden->label));
      diag_note(diags, (Span){0},
                "declared '" SV_FMT "' in '" SV_FMT "'; `pub mod " SV_FMT
                ";` would make it public",
                SV_ARG(hidden->name),
                SV_ARG(mod_display_path(hidden->parent, al)),
                SV_ARG(hidden->name));
      ok = false;
      continue;
    }

    // With the module settled, what follows it is arithmetic rather than a
    // question about the filesystem: nothing is an item, one segment is an
    // item, two are an enum and one of its variants.
    // a glob is the same arithmetic one reading further: nothing left over
    // names the module's own items, one segment names an enum's variants.
    if (glob && rest > 1) {
      diag_error(diags, decl->as.use_decl.target.span,
                 "a glob import names the items of a module "
                 "(`use <module>::*;`) or the variants of one of its enums "
                 "(`use <module>::<Enum>::*;`)");
      ok = false;
      continue;
    }
    int max_rest = bare ? 2 : 1;
    if (rest > max_rest) {
      diag_error(diags, path->span,
                 "too many segments in use path: '" SV_FMT
                 "' names an item of module '" SV_FMT
                 "', or a variant of one of its enums",
                 SV_ARG(sprint_mod_path(path, path->count, al)),
                 SV_ARG(cur->label));
      ok = false;
      continue;
    }

    // the enum a variant import is qualified by is the last segment the module
    // did not account for, whichever shape wrote it down — so it sits one past
    // the module in every case, and only the longest shape has one at all.
    if (rest == max_rest) {
      decl->as.use_decl.qualifier = path->segments[consumed].name;
    }
    decl->as.use_decl.is_module_import = bare && rest == 0;

    imp->file_path = cur->file_path;
    imp->module_index = cur->reg_index;
    assert(imp->module_index >= 0 && "walked to an unregistered module");
  }

  return ok;
}

int mod_split_path(const char *path, StringView *out, int max) {
  StringView p = sv_from_cstr(path);
  int n = 0;
  int i = 0;
  while (i <= p.len && n < max) {
    int start = i;
    while (i < p.len &&
           !(p.chars[i] == ':' && i + 1 < p.len && p.chars[i + 1] == ':')) {
      i++;
    }
    out[n++] = (StringView){.chars = p.chars + start, .len = i - start};
    i += 2; // past the "::"
  }
  return n;
}

// ── the prelude ────────────────────────────────────────────────────────────
//
// ducktape has no auto-import, so a construct whose meaning lives in std used
// to need an import the user never wrote: interpolating a struct needed
// `std::fmt` (for `Display`), `<` on a struct needed `std::cmp` (for `Ord`),
// a `{v:>8}` spec needed `std::string`. Each is a "lang item" — a name the
// *compiler* resolves, so leaving it to whatever happens to be imported is the
// friction this prelude removes. The set is exactly the lang-item modules plus
// the two vocabulary enums:
//
//   std::option  → Option        (vocabulary; the type `for`/`?` speak)
//   std::result  → Result        (its pair)
//   std::cmp     → Ord           (ordering operators; a bare `T: Ord` bound)
//   std::fmt     → Display        (interpolation; captures `float` too)
//   std::iter    → Iterator       (`for x in it`; captures the trait lang item)
//   std::string  → (nothing)     (capture-only: the `pad_*` a width spec needs)
//   std::ops     → Add/Sub/…     (arithmetic operators; the bounds a generic
//                                 doing arithmetic has to write)
//
// `print` is deliberately *not* here: it is an ordinary function, tied to no
// syntax, so it stays an explicit `use std::io::print`.
typedef struct {
  const char *module;   // std leaf name
  const char *items[7]; // pub items to bind; NULL-terminated, may be empty
} PreludeEntry;

static const PreludeEntry prelude[] = {
    // the two enums bring their variants: `Some(v)`, `None`, `Ok(v)`, `Err(e)`
    // are written bare, which is what the prelude is for — a name the language
    // itself reaches for (`?` builds an `Err`, `for` unwraps a `Some`) should
    // not need an import to be *written*. Like every prelude name they are
    // lowest priority, so a module declaring its own `Ok` still wins.
    {"option", {"Option", "Some", "None", NULL}},
    {"result", {"Result", "Ok", "Err", NULL}},
    {"cmp", {"Ord", NULL}},
    {"fmt", {"Display", NULL}},
    {"iter", {"Iterator", NULL}},
    {"string", {NULL}}, // capture-only, for the pad_* / spec lang items
    // All six are bound, not just captured: unlike `Display` and `Ord`, whose
    // bounds a program can usually leave to a call, a generic that does
    // arithmetic must *write* `T: Add` — so the name has to be in scope for the
    // diagnostic asking for it to be followable.
    {"ops", {"Add", "Sub", "Mul", "Div", "Rem", "Neg", NULL}},
};
#define PRELUDE_COUNT ((int)(sizeof(prelude) / sizeof(prelude[0])))

// Inject the prelude into `m` as ordinary synthesised imports: a `use`-less
// user module still depends on every prelude module and binds its items. std
// modules are exempt (a prelude module cannot import itself — that is the
// cycle the exemption avoids), and the entries carry `from_prelude` so linking
// yields silently to a local decl or an explicit import of the same name.
// Appended *after* the real imports so those link first and take priority.
static void reach_prelude(ModuleRegistry *reg) {
  for (int i = 0; i < PRELUDE_COUNT; i++) {
    ModChild *ch =
        mod_find_child(reg->std_root, sv_from_cstr(prelude[i].module));
    assert(ch != NULL && "std/lib.dt does not declare a prelude module");
    modreg_add(reg, ch->mod);
  }
}

void mod_inject_prelude(Module *m, ModuleRegistry *reg, Allocator *al) {
  if (mod_is_std_module(m)) {
    return;
  }

  int old = m->import_count;
  m->imports = al_realloc(al, m->imports, sizeof(ModImport) * (size_t)old,
                          sizeof(ModImport) * (size_t)(old + PRELUDE_COUNT));

  for (int i = 0; i < PRELUDE_COUNT; i++) {
    const PreludeEntry *e = &prelude[i];
    ModChild *ch = mod_find_child(reg->std_root, sv_from_cstr(e->module));
    assert(ch != NULL && "std/lib.dt does not declare a prelude module");
    int dep = ch->mod->reg_index;
    assert(dep >= 0 && "a prelude module was never reached");

    int item_count = 0;
    while (e->items[item_count] != NULL) {
      item_count++;
    }

    Decl *use = al_alloc_zero_for(al, Decl);
    use->kind = DECL_USE;
    use->as.use_decl.from_prelude = true;
    use->as.use_decl.is_module_import = false;
    use->as.use_decl.target.count = item_count;
    if (item_count > 0) {
      UseAlias *aliases =
          al_alloc_zero(al, sizeof(UseAlias) * (size_t)item_count);
      for (int j = 0; j < item_count; j++) {
        aliases[j].name = sv_from_cstr(e->items[j]);
        aliases[j].alias = aliases[j].name; // no `as` rename
      }
      use->as.use_decl.target.aliases = aliases;
    }

    m->imports[m->import_count++] = (ModImport){
        .decl = use, .file_path = ch->mod->file_path, .module_index = dep};
  }
}

bool mod_parse(Module *m, DiagBag *diags, Allocator *al) {
  // ── where the bytes come from ─────────────────────────────────────────────
  // Everything downstream of here — scanning, parsing, the dependency graph,
  // cycle detection, `pub`, linking, codegen, serialization — works off
  // `m->source` and treats `file_path` as an opaque key and a diagnostic
  // label. Pointing this branch at a directory instead of the embedded table
  // is all it would take to make std filesystem-backed.
  //
  // `from_disk` is the lint hatch, and the reason it is a flag rather than a
  // path shape: a library module read off the filesystem is still the library
  // module it was declared to be, so only where its bytes come from changes.
  if (m->is_std && !m->from_disk) {
    const char *src = std_module_source(m->std_rel);
    assert(src != NULL && "std module registered without embedded source");
    m->source = (String){.chars = (char *)src, .len = (int)strlen(src)};
  } else {
    m->source = read_file(m->file_path.chars, al);
  }
  if (m->source.chars == NULL) {
    return false;
  }

  diag_clear(diags);
  Scanner scanner;
  scanner_init(&scanner, m->source.chars, diags);

  Token *tokens = NULL;
  int token_count = scanner_tokenise_all(&scanner, &tokens, al);
  assert(tokens != NULL);

  if (diag_has_errors(diags)) {
    return false;
  }

  Parser parser;
  parser_init(&parser, tokens, token_count, al, diags);
  m->ast = parser_parse(&parser);
  assert(m->ast != NULL);

  if (diag_has_errors(diags)) {
    return false;
  }

  return true;
}

static String read_file(const char *path, Allocator *al) {
  FILE *file = fopen(path, "rb");
  if (file == NULL) {
    fprintf(stderr, "could not open file '%s'.\n", path);
    return str_null();
  }

  fseek(file, 0L, SEEK_END);
  size_t file_size = ftell(file);
  rewind(file);

  char *buffer = al_alloc(al, sizeof(char) * (file_size + 1));
  if (buffer == NULL) {
    fprintf(stderr, "not enough memory to read '%s'.\n", path);
    fclose(file);
    return str_null();
  }

  size_t bytes_read = fread(buffer, sizeof(char), file_size, file);
  if (bytes_read < file_size) {
    fprintf(stderr, "could not read file '%s'.\n", path);
    al_free(al, buffer, sizeof(char) * (file_size + 1));
    fclose(file);
    return str_null();
  }

  buffer[bytes_read] = '\0';
  fclose(file);

  // the first of the two untrusted intakes (the other is an image's string
  // table). A literal has no `\u{}` escape, so raw source bytes are the only
  // route a string has to anything non-ASCII — checking the file once here is
  // what lets every string downstream be valid UTF-8 by construction. The
  // embedded std does not come through here and is trusted rather than checked,
  // which is a wart and written down as one.
  if (!utf8_validate(buffer, (int)bytes_read)) {
    fprintf(stderr, "'%s' is not valid UTF-8.\n", path);
    al_free(al, buffer, sizeof(char) * (file_size + 1));
    return str_null();
  }

  return str_create(buffer, (int)bytes_read, (int)file_size + 1);
}

void modreg_init(ModuleRegistry *reg, Allocator *al) {
  reg->modules = NULL;
  reg->module_count = 0;
  reg->module_cap = 0;
  reg->program_root = NULL;
  reg->std_root = NULL;
  reg->al = al;
}

void modreg_destroy(ModuleRegistry *reg) {
  for (int i = 0; i < reg->module_count; i++) {
    mod_free(&reg->modules[i], reg->al);
  }
  al_free(reg->al, reg->modules, sizeof(Module *) * reg->module_cap);
}

int modreg_add(ModuleRegistry *reg, Module *m) {
  // the tree mints each module exactly once, so this is idempotence rather
  // than dedup: a library module is registered by whichever path reaches it
  // first, and every later one finds it already in the build.
  if (m->reg_index >= 0) {
    return m->reg_index;
  }

  // grow the array if needed
  if (reg->module_count >= reg->module_cap) {
    int new_cap = reg->module_cap == 0 ? 8 : reg->module_cap * 2;
    reg->modules =
        al_realloc(reg->al, reg->modules, sizeof(Module *) * reg->module_cap,
                   sizeof(Module *) * new_cap);
    reg->module_cap = new_cap;
  }

  reg->modules[reg->module_count] = m;
  m->reg_index = reg->module_count;
  return reg->module_count++;
}
