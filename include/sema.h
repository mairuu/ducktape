#pragma once

#include "allocator.h"
#include "ast.h"
#include "diag.h"

typedef struct Module Module;
typedef struct TypeChecker TypeChecker;
typedef struct ValueScope ValueScope;
typedef struct TypeScope TypeScope;
typedef struct TypeResolver TypeResolver;
typedef struct ResolveCtx ResolveCtx;
typedef struct CheckCtx CheckCtx;

// ═══════════════════════════════════════════════════════════════════════════════
// TypeChecker
// ═══════════════════════════════════════════════════════════════════════════════

struct TypeChecker {
  // StructDef **structs;
  // int struct_count, struct_cap;

  // EnumDef **enums;
  // int enum_count, enum_cap;

  // TraitDef **traits;
  // int trait_count, trait_cap;

  // ImplDef **impls;
  // int impl_count, impl_cap;

  FunDef **funs;
  int fun_count, fun_cap;

  Type *t_int, *t_float, *t_bool, *t_string, *t_unit, *t_poison;

  DiagBag *diags;
  Allocator *al;
};

void tc_init(TypeChecker *tc, DiagBag *diags, Allocator *al);

void tc_destroy(TypeChecker *tc);

void tc_register_module(TypeChecker *tc, Module *m);

bool tc_resolve_module(TypeChecker *tc, Module *m);

bool tc_check_module(TypeChecker *tc, Module *m);

// ═══════════════════════════════════════════════════════════════════════════════
// ValueScope
// ═══════════════════════════════════════════════════════════════════════════════

typedef struct {
  StringView name;
  Type *type;
  int slot;         // local slot or global slot (< 0)
  bool is_captured; // set when a nested closure captures this binding
} VarEntry;

struct ValueScope {
  VarEntry *entries;
  int count;
  int cap;

  ValueScope *parent;
  bool is_fn_boundary; // true for the outermost scope of a fun/closure
  bool is_loop;        // true if directly inside a for/while body
  int next_slot;       // next slot to assign (advanced by vscope_define)
  Allocator *al;
};

void vscope_init(ValueScope *scope, ValueScope *parent, Allocator *al);

// push a new scope. next_slot is inherited from parent unless is_fn_boundary.
ValueScope *vscope_push(ValueScope *parent, bool is_fn_boundary, bool is_loop,
                        Allocator *al);

// pop this scope, returning its parent. does not free (arena-allocated).
ValueScope *vscope_pop(ValueScope *scope);

// walk the parent chain.  sets *out_crossed_fn if a fn boundary was crossed.
// returns null if not found.
VarEntry *vscope_lookup(ValueScope *scope, StringView name,
                        bool *out_crossed_fn);

// define a new binding; assigns the next slot.  returns the assigned slot.
// emits a diagnostic and returns -1 if the name already exists in this scope.
int vscope_define(ValueScope *scope, StringView name, Type *type,
                  DiagBag *diags, Span span);

// ═══════════════════════════════════════════════════════════════════════════════
// TypeScope
// ═══════════════════════════════════════════════════════════════════════════════

typedef struct {
  StringView name;
  Type *type;
} TypeEntry;

struct TypeScope {
  TypeEntry *entries;
  int count;
  int cap;

  TypeScope *parent;

  Allocator *al;
};

TypeScope *tscope_push(TypeScope *parent, Allocator *al);

TypeScope *tscope_pop(TypeScope *scope);

// walk parent chain; return null if not found.
TypeEntry *tscope_lookup(TypeScope *scope, StringView name);

// define in the current (top) scope.
// emits a diagnostic if the name is already defined in this exact scope.
void tscope_define(TypeScope *scope, StringView name, Type *type,
                   DiagBag *diags, Span span);

// convenience: push a new scope, create a TY_GENERIC for every TypeParamNode,
// and define them.  Bounds are left empty; call tyres_resolve_bounds()
// afterwards to fill them in once the type resolver is available.
// returns the new scope.
TypeScope *tscope_open_params(TypeScope *parent, TypeParamNode *params,
                              int count, Allocator *al);

// ═══════════════════════════════════════════════════════════════════════════════
// TypeResolver
// ═══════════════════════════════════════════════════════════════════════════════

struct TypeResolver {
  TypeChecker *tc;
  TypeScope *tscope;

  DiagBag *diags;
  Allocator *al;
};

// Resolve `node`, write node->resolved, and return it.
Type *tyres_resolve(TypeResolver *r, TypeNode *node);

// ═══════════════════════════════════════════════════════════════════════════════
// ResolveCtx
// ═══════════════════════════════════════════════════════════════════════════════

struct ResolveCtx {
  TypeChecker *tc;

  TypeResolver tyres;

  DiagBag *diags;
  Allocator *al;
};

void rctx_init(ResolveCtx *rctx, TypeChecker *tc, DiagBag *diags,
               Allocator *al);

static inline Type *rctx_resolve(ResolveCtx *ctx, TypeNode *node) {
  return tyres_resolve(&ctx->tyres, node);
}

// ═══════════════════════════════════════════════════════════════════════════════
// ResolveCtx
// ═══════════════════════════════════════════════════════════════════════════════

struct CheckCtx {
  TypeChecker *tc;

  ValueScope *vscope;
  TypeScope *tscope;

  TypeResolver tyres;

  DiagBag *diags;
  Allocator *al;
};

void cctx_init(CheckCtx *cctx, TypeChecker *tc, DiagBag *diags, Allocator *al);

void cctx_open_module(CheckCtx *cctx, Module *m);

void cctx_open_fun(CheckCtx *ctx, ParamDef *params, int count);