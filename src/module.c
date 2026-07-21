#include "module.h"
#include "allocator.h"
#include "diag.h"
#include "parser.h"
#include "scanner.h"
#include "sema.h"
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

  StringView joined = {.chars = buf, .len = n};
  StringView result = path_normalise(joined, al);
  al_free(al, buf, sizeof(char) * (size_t)(len + 1));
  return result;
}

Module *mod_new(StringView file_path, Allocator *al) {
  Module *m = al_alloc_zero_for(al, Module);
  m->file_path = file_path;
  vscope_init(&m->vscope, NULL, al);
  tscope_init(&m->tscope, NULL, al);
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

// render a use path back as `a::b` for diagnostics.
static void sprint_mod_path(char *buf, int cap, const Path *path) {
  int n = 0;
  for (int i = 0; i < path->count && n < cap - 1; i++) {
    if (i > 0 && n < cap - 3) {
      buf[n++] = ':';
      buf[n++] = ':';
    }
    StringView seg = path->segments[i].name;
    for (int j = 0; j < seg.len && n < cap - 1; j++) {
      buf[n++] = seg.chars[j];
    }
  }
  buf[n] = '\0';
}

bool mod_collect_imports(Module *m, ModuleRegistry *reg, StringView base_dir,
                         DiagBag *diags, Allocator *al) {
  assert(m->ast != NULL);

  for (int i = 0; i < m->ast->decl_count; i++) {
    if (m->ast->decls[i]->kind == DECL_USE) {
      m->import_cap++;
    }
  }
  if (m->import_cap == 0) {
    return true;
  }
  m->imports = al_alloc_zero(al, sizeof(ModImport) * m->import_cap);

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
    for (int j = 0; j < path->count; j++) {
      if (path->segments[j].type_arg_count != 0) {
        bad_args = true;
      }
    }
    if (bad_args) {
      diag_error(diags, path->span,
                 "type arguments are not allowed in a use path");
      ok = false;
      continue;
    }

    // `std` is a reserved namespace over the builtins: no file, no graph edge.
    if (path->count > 0 && sv_equal_cstr(path->segments[0].name, "std")) {
      imp->is_std = true;
      continue;
    }

    imp->file_path = mod_file_for_use(base_dir, path, al);

    Module *dep = modreg_find(reg, imp->file_path);
    if (dep == NULL) {
      if (!file_exists(imp->file_path.chars)) {
        char buf[128];
        sprint_mod_path(buf, sizeof(buf), path);
        diag_error(diags, path->span, "cannot find module '%s'", buf);
        diag_note(diags, (Span){0}, "expected file '" SV_FMT "'",
                  SV_ARG(imp->file_path));
        ok = false;
        continue;
      }
      dep = mod_new(imp->file_path, al);
      modreg_add(reg, dep);
    }
    imp->module_index = modreg_index_of(reg, dep);
  }

  return ok;
}

bool mod_parse(Module *m, DiagBag *diags, Allocator *al) {
  m->source = read_file(m->file_path.chars, al);
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

Module *modreg_find(ModuleRegistry *reg, StringView path) {
  for (int i = 0; i < reg->module_count; i++) {
    if (sv_equal(reg->modules[i]->file_path, path)) {
      return reg->modules[i];
    }
  }
  return NULL;
}

int modreg_index_of(ModuleRegistry *reg, Module *m) {
  for (int i = 0; i < reg->module_count; i++) {
    if (reg->modules[i] == m) {
      return i;
    }
  }
  return -1;
}

bool modreg_add(ModuleRegistry *reg, Module *m) {
  if (modreg_find(reg, m->file_path) != NULL) {
    return false;
  }

  // grow the array if needed
  if (reg->module_count >= reg->module_cap) {
    int new_cap = reg->module_cap == 0 ? 8 : reg->module_cap * 2;
    reg->modules =
        al_realloc(reg->al, reg->modules, sizeof(Module *) * reg->module_cap,
                   sizeof(Module *) * new_cap);
    reg->module_cap = new_cap;
  }

  reg->modules[reg->module_count++] = m;
  return true;
}