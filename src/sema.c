#include "sema.h"
#include "allocator.h"
#include "ast.h"
#include "module.h"

#include <assert.h>

void tc_init(TypeChecker *tc, DiagBag *diags, Allocator *al) {
  memset(tc, 0, sizeof(*tc));

  tc->t_int = ty_int();
  tc->t_float = ty_float();
  tc->t_bool = ty_bool();
  tc->t_string = ty_string();
  tc->t_unit = ty_unit();
  tc->t_poison = ty_poison();

  tc->diags = diags;
  tc->al = al;
}

static void tc_register_fun(TypeChecker *tc, Module *m, DeclFun *decl) {
  assert(m->fun_cap > m->fun_count && "fun capacity exceeded");

  FunDef *def = al_alloc_zero_for(tc->al, FunDef);
  def->name = decl->name;

  m->funs[m->fun_count++] = def;
}

void tc_register_module(TypeChecker *tc, Module *m) {
  for (int i = 0; i < m->ast->decl_count; i++) {
    switch (m->ast->decls[i]->kind) {
    case DECL_FUN:
      m->fun_cap++;
      break;
    default:
      assert(false && "unhandled decl kind in tc_register_module");
    }
  }

  m->funs = al_alloc_zero(tc->al, sizeof(FunDef *) * m->fun_cap);
  assert(m->funs && "out of memory");

  for (int i = 0; i < m->ast->decl_count; i++) {
    switch (m->ast->decls[i]->kind) {
    case DECL_FUN:
      tc_register_fun(tc, m, &m->ast->decls[i]->as.fun_decl);
      break;
    default:
      assert(false && "unhandled decl kind in tc_register_module");
    }
  }

  assert(m->fun_count == m->fun_cap && "fun count mismatch after registration");
}