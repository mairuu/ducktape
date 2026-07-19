#include "compiler.h"
#include "allocator.h"
#include "arena.h"
#include "ast.h"
#include "diag.h"
#include "module.h"
#include "sema.h"
#include "string_utils.h"

#include <assert.h>

void compiler_init(Compiler *c, Allocator *al) {
  arena_init(&c->arena, al);
  c->al = arena_allocator_create(&c->arena);

  diag_init(&c->diags, &c->al);
  modreg_init(&c->mod_reg, &c->al);
  tc_init(&c->tc, &c->diags, &c->al);
}

void compiler_destroy(Compiler *c, Allocator *al) {
  (void)al;
  arena_destroy(&c->arena);

  diag_destroy(&c->diags);
  modreg_destroy(&c->mod_reg);
  tc_destroy(&c->tc);
}

// phase 0: discover all source files and create module stubs.
static bool compiler_phase_discover(Compiler *c, const char *root_dir) {
  StringView file_paths[] = {
      sv_from_cstr(root_dir),
  };
  int file_count = sizeof(file_paths) / sizeof(file_paths[0]);

  for (int i = 0; i < file_count; i++) {
    StringView path = file_paths[i];

    Module *root = mod_new(path, &c->al);
    assert(modreg_add(&c->mod_reg, root));
  }

  // for now just a single file
  c->root_module = c->mod_reg.modules[0];
  return true;
}

// phase 0: parse every module's source file.
static bool compiler_phase_parse(Compiler *c) {
  bool had_errors = false;
  for (int i = 0; i < c->mod_reg.module_count; i++) {
    Module *m = c->mod_reg.modules[i];
    had_errors |= !mod_parse(m, &c->diags, &c->al);

    if (diag_has_diags(&c->diags)) {
      diag_report(&c->diags, m->file_path.chars, m->source.chars, stderr);
    }
  }
  return !had_errors;
}

// phase 0: build the dependency graph and topologically sort modules.
static bool compiler_phase_dep_graph(Compiler *c) {
  c->mod_reg.topo_count = c->mod_reg.module_count;
  c->mod_reg.topo_order = al_alloc(&c->al, sizeof(int) * c->mod_reg.topo_count);

  for (int i = 0; i < c->mod_reg.module_count; i++) {
    c->mod_reg.topo_order[i] = i;
  }

  return true;
}

// phase 1: register declarations + resolve imports, in topological order.
static bool compiler_phase_register(Compiler *c) {
  bool had_errors = false;
  for (int i = 0; i < c->mod_reg.module_count; i++) {
    Module *m = modreg_topo(&c->mod_reg, i);
    diag_clear(&c->diags);

    tc_register_module(&c->tc, m);
    if (diag_has_diags(&c->diags)) {
      diag_report(&c->diags, m->file_path.chars, m->source.chars, stderr);
    }

    had_errors |= diag_has_errors(&c->diags);
  }

  // todo:
  // for (int i = 0; i < c->mod_reg.module_count; i++) {
  //   Module *m = c->mod_reg.modules[i];
  //   mod_link_imports(m, &c->mod_reg, &c->diags);
  // }

  return !had_errors;
}

// phase 2: resolve all signatures, in topological order.
static bool compiler_phase_resolve(Compiler *c) {
  bool had_errors = false;
  for (int i = 0; i < c->mod_reg.module_count; i++) {
    Module *m = modreg_topo(&c->mod_reg, i);
    diag_clear(&c->diags);

    tc_resolve_module(&c->tc, m);
    if (diag_has_diags(&c->diags)) {
      diag_report(&c->diags, m->file_path.chars, m->source.chars, stderr);
    }

    had_errors |= diag_has_errors(&c->diags);
  }
  return !had_errors;
}

// phase 3: type-check all bodies, in topological order.
static bool compiler_phase_check(Compiler *c) {
  bool had_errors = false;
  for (int i = 0; i < c->mod_reg.module_count; i++) {
    Module *m = modreg_topo(&c->mod_reg, i);
    diag_clear(&c->diags);

    tc_check_module(&c->tc, m);
    if (diag_has_diags(&c->diags)) {
      diag_report(&c->diags, m->file_path.chars, m->source.chars, stderr);
    }
    had_errors |= diag_has_errors(&c->diags);
  }

  return !had_errors;
}

// run the full compilation pipeline
bool compiler_run(Compiler *c, const char *path) {
  if (!compiler_phase_discover(c, path)) {
    fprintf(stderr, "compilation failed during discovery.\n");
    return false;
  }

  if (!compiler_phase_parse(c)) {
    fprintf(stderr, "compilation failed during parsing.\n");
    return false;
  }

  if (!compiler_phase_dep_graph(c)) {
    fprintf(stderr,
            "compilation failed during dependency graph construction.\n");
    return false;
  }

  if (!compiler_phase_register(c)) {
    fprintf(stderr, "compilation failed during registration.\n");
    return false;
  }

  if (!compiler_phase_resolve(c)) {
    fprintf(stderr, "compilation failed during resolution.\n");
    return false;
  }

  if (!compiler_phase_check(c)) {
    fprintf(stderr, "compilation failed during type checking.\n");
    return false;
  }

  return true;
}
