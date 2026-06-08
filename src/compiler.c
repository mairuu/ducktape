#include "compiler.h"
#include "allocator.h"
#include "arena.h"
#include "diag.h"
#include "sema.h"
#include "string_utils.h"

#include <assert.h>

void compiler_init(Compiler *c, Allocator *al) {
  arena_init(&c->arena, al);
  c->al = arena_allocator_create(&c->arena);

  diag_init(&c->diags, &c->al);
  tc_init(&c->tc, &c->diags, &c->al);
}

void compiler_destroy(Compiler *c, Allocator *al) {
  (void)al;
  arena_destroy(&c->arena);

  diag_destroy(&c->diags);
}

// run the full compilation pipeline
// only single file for now
void compiler_run(Compiler *c, const char *path) {
  c->root_module = al_alloc_for(&c->al, Module);
  c->root_module->file_path = sv_from_cstr(path);

  bool parsed = mod_parse(c->root_module, &c->diags, &c->al);
  (void)parsed;
  // assert(parsed && "parsing failed");

  if (diag_has_diags(&c->diags)) {
    diag_report(&c->diags, c->root_module->file_path.chars,
                c->root_module->source.chars, stderr);
  }

  tc_register_module(&c->tc, c->root_module);
}
