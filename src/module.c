#include "module.h"
#include "allocator.h"
#include "diag.h"
#include "parser.h"
#include "scanner.h"
#include "sema.h"
#include "std_src.h"
#include "string_utils.h"

#include <assert.h>
#include <stdio.h>

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

StringView mod_file_for_use(StringView base_dir, const Path *path,
                            Allocator *al) {
  int len = base_dir.len + 3; // ".dt"
  for (int i = 0; i < path->count; i++) {
    len += path->segments[i].name.len + 1; // + '/' separator
  }

  char *buf = al_alloc(al, sizeof(char) * (size_t)(len + 1));
  int n = 0;

  memcpy(buf + n, base_dir.chars, (size_t)base_dir.len);
  n += base_dir.len;

  for (int i = 0; i < path->count; i++) {
    if (i > 0) {
      buf[n++] = '/';
    }
    StringView seg = path->segments[i].name;
    memcpy(buf + n, seg.chars, (size_t)seg.len);
    n += seg.len;
  }

  memcpy(buf + n, ".dt", 3);
  n += 3;
  buf[n] = '\0';

  // already normalised by construction: base_dir is a prefix of a normalised
  // path, and the segments are identifiers, so no "." or ".." can appear.
  return (StringView){.chars = buf, .len = n};
}

// The registry key (and diagnostic label) of an embedded std module. The
// angle brackets are what keep it distinct from every user path: a real one is
// a base dir plus identifier segments, and an identifier cannot contain '<'.
// So a user file literally named `std/cmp.dt` cannot collide with `std::cmp` —
// which is what `mod_adopt_std` then has to undo for the one path that *is*
// the library, since two keys for one file is two modules.
StringView std_mod_key(StringView name, Allocator *al) {
  const char *prefix = "<std>/";
  int prefix_len = 6;
  int len = prefix_len + name.len + 3; // ".dt"

  char *buf = al_alloc(al, sizeof(char) * (size_t)(len + 1));
  memcpy(buf, prefix, (size_t)prefix_len);
  memcpy(buf + prefix_len, name.chars, (size_t)name.len);
  memcpy(buf + prefix_len + name.len, ".dt", 3);
  buf[len] = '\0';

  return (StringView){.chars = buf, .len = len};
}

// is this key one std_mod_key produced? `mod_parse` asks, to decide whether
// the source comes from the embedded table or from the filesystem. That is a
// question about *where the bytes are*, and the only one still asked of the
// path — being a std module is `Module.std_name`, which an adopted root has
// while its path stays a real one.
static bool is_std_key(StringView path) {
  return path.len > 6 && memcmp(path.chars, "<std>/", 6) == 0;
}

// the leaf of a `<std>/<leaf>.dt` key. Caller has checked `is_std_key`.
static StringView std_key_leaf(StringView key) {
  return (StringView){.chars = key.chars + 6,
                      .len = key.len - 6 - 3}; // "<std>/" .. ".dt"
}

// is this the std module of that name? A std Module is an ordinary Module in
// every other respect, deliberately.
bool mod_is_std(const Module *m, const char *name) {
  return sv_equal_cstr(m->std_name, name);
}

// is this any std module? The gate for `@lang`, which is reserved for the
// standard library — a user cannot claim a lang item.
bool mod_is_std_module(const Module *m) { return m->std_name.len > 0; }

StringView std_name_for_entry(StringView path) {
  StringView none = {0};
  if (path.len < 4 || memcmp(path.chars + path.len - 3, ".dt", 3) != 0) {
    return none;
  }

  // split off the leaf; a path with no '/' names no directory, so it cannot
  // spell the library even if the stem matches.
  int slash = -1;
  for (int i = path.len - 1; i >= 0; i--) {
    if (path.chars[i] == '/') {
      slash = i;
      break;
    }
  }
  if (slash < 0) {
    return none;
  }

  StringView stem = {.chars = path.chars + slash + 1,
                     .len = path.len - slash - 1 - 3};
  if (stem.len == 0 || std_module_source(stem) == NULL) {
    return none;
  }

  // the parent component must be exactly `std`
  StringView dir = {.chars = path.chars, .len = slash};
  int dir_slash = -1;
  for (int i = dir.len - 1; i >= 0; i--) {
    if (dir.chars[i] == '/') {
      dir_slash = i;
      break;
    }
  }
  StringView parent = {.chars = dir.chars + dir_slash + 1,
                       .len = dir.len - dir_slash - 1};
  return sv_equal_cstr(parent, "std") ? stem : none;
}

void mod_adopt_std(Module *m, StringView name, Allocator *al) {
  m->std_name = name;
  m->alias_path = std_mod_key(name, al);
}

VariantImport *mod_find_variant_import(Module *m, StringView name) {
  VariantImport *slot = mod_variant_import_slot(m, name);
  // a reservation names nothing yet, and a dead one never will: both read as
  // "no such variant" everywhere outside the linking that owns them.
  return (slot != NULL && slot->variant != NULL) ? slot : NULL;
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

Module *mod_new(StringView file_path, Allocator *al) {
  Module *m = al_alloc_zero_for(al, Module);
  m->file_path = file_path;
  // a `<std>/…` key names a std module by construction; a real path only does
  // so once `mod_adopt_std` says the entry point spelled the library.
  if (is_std_key(file_path)) {
    m->std_name = std_key_leaf(file_path);
  }
  vscope_init(&m->vscope, NULL, al);
  tscope_init(&m->tscope, NULL, al);
  impl_index_init(&m->visible_impls, al);
  return m;
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

// does the file the first `count` segments of `path` name already exist (on
// disk, or as a registry entry)? The one question that splits a use path into
// a module prefix and what follows it.
static bool mod_prefix_exists(ModuleRegistry *reg, StringView base_dir,
                              const Path *path, int count, Allocator *al) {
  Path prefix = *path;
  prefix.count = count;
  StringView file = mod_file_for_use(base_dir, &prefix, al);
  return modreg_find(reg, file) >= 0 || file_exists(file.chars);
}

// render a use path back as `a::b` for diagnostics. exact-sized, so there is
// no bound to juggle and nothing truncates.
static StringView sprint_mod_path(const Path *path, Allocator *al) {
  int len = 0;
  for (int i = 0; i < path->count; i++) {
    len += path->segments[i].name.len + (i > 0 ? 2 : 0);
  }

  char *buf = al_alloc(al, sizeof(char) * (size_t)(len + 1));
  int n = 0;
  for (int i = 0; i < path->count; i++) {
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

bool mod_collect_imports(Module *m, ModuleRegistry *reg, StringView base_dir,
                         DiagBag *diags, Allocator *al) {
  assert(m->ast != NULL);

  // exact-sized: nothing appends to m->imports after this.
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

    // A glob's last segment is *always* the enum — there is no module glob, so
    // nothing else it could be — which makes its module prefix one shorter and
    // spares it the probing the other two shapes need.
    if (glob) {
      decl->as.use_decl.qualifier = path->segments[path->count - 1].name;
    }

    // `std` names the embedded standard library. A std module is exactly two
    // segments (`std::<leaf>`), so a bare `use std::x;` binds that module and a
    // longer `use std::x::item;` imports an item from it. A std module that
    // exists is an ordinary registry entry — same dedup, same graph edge, same
    // `pub` rules — differing from a user module *only* in where mod_parse gets
    // its bytes. Keeping it ordinary is what makes moving std to a real
    // directory later a one-branch change.
    if (path->count > 0 && sv_equal_cstr(path->segments[0].name, "std")) {
      bool is_module = bare && path->count == 2 &&
                       std_module_source(path->segments[1].name) != NULL;
      // a std module is exactly two segments, so the shape of what follows is
      // arithmetic rather than a file question: one segment is an item, two are
      // an enum and its variant.
      int extra = path->count - 2 - (bare ? 1 : 0);
      if (extra > 1) {
        diag_error(diags, path->span,
                   "too many segments in use path: '" SV_FMT
                   "' names an item of a std module, or a variant of one of "
                   "its enums",
                   SV_ARG(sprint_mod_path(path, al)));
        ok = false;
        continue;
      }
      if (glob && extra != 1) {
        diag_error(diags, decl->as.use_decl.target.span,
                   "a glob import names the variants of an enum "
                   "(`use std::<module>::<Enum>::*;`), and there is no module "
                   "glob");
        ok = false;
        continue;
      }
      if (extra == 1) {
        decl->as.use_decl.qualifier =
            path->segments[path->count - 1 - (bare ? 1 : 0)].name;
      }

      StringView mod_name =
          path->count > 1 ? path->segments[1].name : (StringView){0};

      if (std_module_source(mod_name) == NULL) {
        // name the module looked for (`std::<leaf>`), not the whole path — the
        // trailing segments of `use std::nope::thing` are an item spelling.
        Path mod_only = *path;
        if (mod_only.count > 2) {
          mod_only.count = 2;
        }
        StringView mod_path = sprint_mod_path(&mod_only, al);
        diag_error(diags, path->span,
                   "there is no standard library module '" SV_FMT "'",
                   SV_ARG(mod_path));
        diag_note(diags, (Span){0}, "available: %s", std_module_names());
        ok = false;
        continue;
      }

      decl->as.use_decl.is_module_import = is_module;
      imp->file_path = std_mod_key(mod_name, al);
      int dep = modreg_find(reg, imp->file_path);
      if (dep < 0) {
        dep = modreg_add(reg, mod_new(imp->file_path, al));
      }
      imp->module_index = dep;
      continue;
    }

    // A bare use whose full path names a module file binds that module;
    // anything else imports the trailing name from its prefix module, and one
    // segment further back may be the enum that name is a variant of. Which is
    // which is the same file-existence question each time, asked shortest-
    // prefix-last so an item import keeps winning over a variant one.
    // `module_path` is the file to load in every case.
    // The leading segment may also name no file at all, but an *enum in this
    // module's scope* — its own, one it imported, or a preluded one:
    // `use Event::A;`, `use Color::Red;`, `use Option::Some;`. That is the last
    // reading of all, taken only where no file answered, so nothing that
    // resolved before resolves differently. It carries no dependency edge
    // (`module_index` stays -1) and binds after this module resolves, since a
    // scope is only complete then — `tc_link_scope_imports`.
    int scope_prefix = bare ? 2 : 1;
    if (path->count == scope_prefix &&
        !mod_prefix_exists(reg, base_dir, path, path->count, al) &&
        !mod_prefix_exists(reg, base_dir, path, 1, al)) {
      decl->as.use_decl.is_scope_import = true;
      decl->as.use_decl.qualifier = path->segments[0].name;
      // the file the *module* reading would have named, for the diagnostic the
      // link pass emits when the qualifier turns out to be no enum either.
      Path mod_only = *path;
      mod_only.count = 1;
      imp->file_path = mod_file_for_use(base_dir, &mod_only, al);
      continue;
    }

    Path module_path = *path;
    bool is_module = false;
    if (glob) {
      // the last segment is the enum, so the prefix is everything before it —
      // and a prefix that names no file means the whole path was a module,
      // which a glob cannot take.
      module_path.count = path->count - 1;
      if (!mod_prefix_exists(reg, base_dir, &module_path, module_path.count,
                             al) &&
          mod_prefix_exists(reg, base_dir, path, path->count, al)) {
        diag_error(diags, decl->as.use_decl.target.span,
                   "a glob import names the variants of an enum "
                   "(`use <module>::<Enum>::*;`), and there is no module glob");
        ok = false;
        continue;
      }
    } else if (bare) {
      StringView full = mod_file_for_use(base_dir, path, al);
      if (file_exists(full.chars)) {
        is_module = true; // the whole path is a module
      } else if (path->count >= 2) {
        module_path.count = path->count - 1; // trailing name is an item
        if (path->count >= 3 &&
            !mod_prefix_exists(reg, base_dir, path, path->count - 1, al) &&
            mod_prefix_exists(reg, base_dir, path, path->count - 2, al)) {
          module_path.count = path->count - 2;
          decl->as.use_decl.qualifier = path->segments[path->count - 2].name;
        }
      } else {
        // a one-segment bare use has no prefix to fall back on.
        StringView mod_path = sprint_mod_path(path, al);
        diag_error(diags, path->span, "cannot find module '" SV_FMT "'",
                   SV_ARG(mod_path));
        diag_note(diags, (Span){0}, "expected file '" SV_FMT "'", SV_ARG(full));
        ok = false;
        continue;
      }
    } else if (path->count >= 2 &&
               !mod_prefix_exists(reg, base_dir, path, path->count, al) &&
               mod_prefix_exists(reg, base_dir, path, path->count - 1, al)) {
      // braced: the prefix names the file, so a trailing segment it does not
      // account for is the enum the braced names are variants of.
      module_path.count = path->count - 1;
      decl->as.use_decl.qualifier = path->segments[path->count - 1].name;
    }

    decl->as.use_decl.is_module_import = is_module;
    imp->file_path = mod_file_for_use(base_dir, &module_path, al);

    int dep = modreg_find(reg, imp->file_path);
    if (dep < 0) {
      if (!file_exists(imp->file_path.chars)) {
        StringView mod_path = sprint_mod_path(&module_path, al);
        diag_error(diags, path->span, "cannot find module '" SV_FMT "'",
                   SV_ARG(mod_path));
        diag_note(diags, (Span){0}, "expected file '" SV_FMT "'",
                  SV_ARG(imp->file_path));
        ok = false;
        continue;
      }
      dep = modreg_add(reg, mod_new(imp->file_path, al));
    }
    imp->module_index = dep;
  }

  return ok;
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
void mod_inject_prelude(Module *m, ModuleRegistry *reg, Allocator *al) {
  if (mod_is_std_module(m)) {
    return;
  }

  int old = m->import_count;
  m->imports = al_realloc(al, m->imports, sizeof(ModImport) * (size_t)old,
                          sizeof(ModImport) * (size_t)(old + PRELUDE_COUNT));

  for (int i = 0; i < PRELUDE_COUNT; i++) {
    const PreludeEntry *e = &prelude[i];
    StringView key = std_mod_key(sv_from_cstr(e->module), al);
    int dep = modreg_find(reg, key);
    if (dep < 0) {
      dep = modreg_add(reg, mod_new(key, al));
    }

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

    m->imports[m->import_count++] =
        (ModImport){.decl = use, .file_path = key, .module_index = dep};
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
  // The question is the *path*, not `std_name`: an adopted root is a std
  // module read from disk, which is what makes it a lint of the file being
  // edited rather than of whatever `make` last mirrored.
  if (is_std_key(m->file_path)) {
    const char *src = std_module_source(std_key_leaf(m->file_path));
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
    return str_null();
  }

  size_t bytes_read = fread(buffer, sizeof(char), file_size, file);
  if (bytes_read < file_size) {
    fprintf(stderr, "could not read file '%s'.\n", path);
    al_free(al, buffer, sizeof(char) * (file_size + 1));
    return str_null();
  }

  buffer[bytes_read] = '\0';
  fclose(file);
  return str_create(buffer, (int)bytes_read, (int)file_size + 1);
}

void modreg_init(ModuleRegistry *reg, Allocator *al) {
  reg->modules = NULL;
  reg->module_count = 0;
  reg->module_cap = 0;
  reg->al = al;
}

void modreg_destroy(ModuleRegistry *reg) {
  for (int i = 0; i < reg->module_count; i++) {
    mod_free(&reg->modules[i], reg->al);
  }
  al_free(reg->al, reg->modules, sizeof(Module *) * reg->module_cap);
}

int modreg_find(ModuleRegistry *reg, StringView path) {
  for (int i = 0; i < reg->module_count; i++) {
    Module *m = reg->modules[i];
    // the alias is the whole of what adoption does to discovery: a later
    // `use std::cmp` resolves its key here and finds the module already
    // registered, so no second one is ever minted.
    if (sv_equal(m->file_path, path) ||
        (m->alias_path.len > 0 && sv_equal(m->alias_path, path))) {
      return i;
    }
  }
  return -1;
}

int modreg_add(ModuleRegistry *reg, Module *m) {
  int existing = modreg_find(reg, m->file_path);
  if (existing >= 0) {
    return existing;
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
  return reg->module_count++;
}