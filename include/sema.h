#pragma once

#include "allocator.h"
#include "ast.h"
#include "diag.h"

typedef struct Module Module;
typedef struct Subst Subst;
typedef struct InferCtx InferCtx;
typedef struct TypeChecker TypeChecker;
typedef struct ValueScope ValueScope;
typedef struct TypeScope TypeScope;
typedef struct ImplIndex ImplIndex;
typedef struct TypeResolver TypeResolver;
typedef struct ResolveCtx ResolveCtx;
typedef struct CheckCtx CheckCtx;

// ═══════════════════════════════════════════════════════════════════════════════
// Substitution
// ═══════════════════════════════════════════════════════════════════════════════

struct Subst {
  StringView *params; // param names, e.g. ["T", "U", "V"]
  Type **args;        // replacement types — parallel array, same length
  int count;
};

static inline Subst subst_empty(void) { return (Subst){0}; }

// build a substitution from two parallel arrays of equal length.
void subst_init(Subst *s, StringView *params, Type **args, int count);

// recursively replace TY_GENERIC nodes whose .name matches an entry.
// unmatched generics pass through unchanged.
Type *subst_apply(const Subst *s, Type *t, Allocator *al);

// instantiate a generic item: for each TY_GENERIC in `type_params`, create
// a fresh TY_UNKNOWN or use the provided type argument (in case of explicit
// type arguments) and build a Subst. after unification, apply infer_apply to
// the substituted return type to read out the inferred type arguments.
Subst infer_open_generics(InferCtx *ctx, Type **type_params,
                          Type **type_args /* nullable */, int param_count,
                          Span span, Allocator *al);

// ═══════════════════════════════════════════════════════════════════════════════
// ImplIndex
// ═══════════════════════════════════════════════════════════════════════════════

typedef struct {
  ImplDef *impl;
  Subst subst;
} ImplMatch;

struct ImplIndex {
  ImplDef **all; // flat list; linear search for now
  int count;
  int cap;
  Allocator *al;
};

void impl_index_init(ImplIndex *idx, Allocator *al);

// register an impl in the index. call once the impl's self_type and methods
// are fully resolved.
void impl_index_add(ImplIndex *idx, ImplDef *impl);

// find a method named `name` applicable to `self_type`, trying every
// registered impl. on a match, *out_match is filled with the impl and the
// substitution mapping the impl's type params to the concrete type args
// inferred from `self_type` (empty subst if the impl isn't generic).
// find a method for `self_type`. when `self_type` is a generic type's
// canonical self (a bare path like `Point::new` with no type args) and
// `infer` is non-NULL, the impl is selected by method name instead and its
// type params are opened as fresh unknowns for the call site to solve.
MethodDef *impl_index_method(ImplIndex *idx, Type *self_type, StringView name,
                             ImplMatch *out_match, InferCtx *infer, Span span,
                             Allocator *al);

// ═══════════════════════════════════════════════════════════════════════════════
// Inference
// ═══════════════════════════════════════════════════════════════════════════════

struct InferCtx {
  Type **solutions; // NULL=free, TY_UNKNOWN=redirect, else solved
  Type **nodes;     // the original TY_UNKNOWN node (never changes)
  int cap;          // allocated slot count
  uint32_t next_id; // next fresh id
  Allocator *al;
};

void infer_init(InferCtx *ctx, Allocator *al);

// allocate a fresh TY_UNKNOWN with an optional trait bound.
Type *infer_fresh(InferCtx *ctx, StringView param_name, Type *bound,
                  Span intro_span);

// walk redirects (with path compression) to the canonical root.
Type *infer_find(InferCtx *ctx, Type *ty);

// structurally unify two types.
//   • If either side is a free TY_UNKNOWN, bind it to the other.
//   • If both are concrete and compatible, recurse into children.
//   • On mismatch, emit a diagnostic and return false.
// callers should treat a false return as a signal to propagate ty_poison().
bool infer_unify(InferCtx *ctx, Type *a, Type *b, DiagBag *diags, Span span);

// convenience: unify a TY_UNKNOWN with a known expected type and return
// the resolved type (or ty_poison() on failure).
Type *infer_expect(InferCtx *ctx, Type *inferred, Type *expected,
                   DiagBag *diags, Span span);

// deeply apply all current solutions. free TY_UNKNOWNs stay as-is;
// the caller can call infer_finalize afterwards to report them as errors.
Type *infer_apply(InferCtx *ctx, Type *ty, Allocator *al);

// After checking a function body: ensure every TY_UNKNOWN is solved.
// emits "type annotation needed" for any that remain free.
void infer_finalize(InferCtx *ctx, DiagBag *diags);

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

  ImplIndex impl_index;

  Type *t_int, *t_float, *t_bool, *t_string, *t_unit, *t_never, *t_poison;

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
  union {
    FunDef *fun; // when type.kind == TY_FUN
  } as;          // payload for certain kinds of entries
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
                  DiagBag *diags, Span span, VarEntry **ref /*nullable*/);

// ═══════════════════════════════════════════════════════════════════════════════
// TypeScope
// ═══════════════════════════════════════════════════════════════════════════════

typedef struct {
  StringView name;
  Type *type;
  union {
    FunDef *fun_def;       // when type.kind == TY_FUN
    StructDef *struct_def; // when type.kind == TY_STRUCT
    EnumDef *enum_def;     // when type.kind == TY_ENUM
    TraitDef *trait_def;   // when type.kind == TY_TRAIT
  } as;                    // payload for certain kinds of entries
} TypeEntry;

struct TypeScope {
  TypeEntry *entries;
  int count;
  int cap;

  TypeScope *parent;

  Allocator *al;
};

void tscope_init(TypeScope *scope, TypeScope *parent, Allocator *al);

TypeScope *tscope_push(TypeScope *parent, Allocator *al);

TypeScope *tscope_pop(TypeScope *scope);

// walk parent chain; return null if not found.
TypeEntry *tscope_lookup(TypeScope *scope, StringView name);

// define in the current (top) scope.
// emits a diagnostic if the name is already defined in this exact scope.
void tscope_define(TypeScope *scope, StringView name, Type *type,
                   DiagBag *diags, Span span, TypeEntry **ref /*nullable*/);

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
  InferCtx *infer; // null during resolve, non-null during check
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
// CheckCtx
// ═══════════════════════════════════════════════════════════════════════════════

struct CheckCtx {
  TypeChecker *tc;

  // current function
  int loop_depth; // 0 when not in a loop, > 0 inside for/while bodies
  FunDef *fun;
  Type *return_type; // expected return

  // type inference
  InferCtx infer;

  ValueScope *vscope;
  TypeScope *tscope;

  TypeResolver tyres;

  DiagBag *diags;
  Allocator *al;
};

void cctx_init(CheckCtx *cctx, TypeChecker *tc, Module *m, DiagBag *diags,
               Allocator *al);

typedef enum {
  PATHRES_METHOD,
  PATHRES_TYPE,
  PATHRES_VAR,
  PATHRES_VARIANT,
} PathResKind;

typedef struct {
  PathResKind kind;
  Type *type;
  union {
    struct {
      FunDef *fun;
      Subst subst; // impl-level substitution (may map to fresh unknowns)
    } method;
    struct {
      EnumDef *enum_def;
      VariantDef *def;
    } variant;
  } as;
} PathRes;

typedef struct {
  Path *path;
  TypeScope *tscope;
  // ValueScope *vscope;
  TypeResolver *tyres;
  DiagBag *diags;
  Allocator *al;
} PathResCtx;

bool resolve_path(PathResCtx *ctx, PathRes *out_res);

bool rctx_resolve_path(ResolveCtx *ctx, Path *path, PathRes *out_res);

bool cctx_resolve_path(CheckCtx *ctx, Path *path, PathRes *out_res);