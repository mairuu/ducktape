#include "sema.h"
#include "allocator.h"
#include "ast.h"
#include "diag.h"
#include "module.h"
#include "string_utils.h"

#include <assert.h>
#include <stdio.h>

// ═══════════════════════════════════════════════════════════════════════════════
// Utilities
// ═══════════════════════════════════════════════════════════════════════════════

#define TYPE_SCRATCH_CAP 8

typedef struct {
  Type *buf[TYPE_SCRATCH_CAP];
  Type **ptr;
  int count;
} TypeScratch;

static void ts_init(TypeScratch *ts, int count, Allocator *al) {
  ts->count = count;
  ts->ptr = count <= TYPE_SCRATCH_CAP ? count == 0 ? NULL : ts->buf
                                      : al_alloc(al, sizeof(Type *) * count);
  memset(ts->ptr, 0, sizeof(Type *) * count);
  assert((count == 0 || ts->ptr) && "out of memory");
}

// ═══════════════════════════════════════════════════════════════════════════════
// AST helper
// ═══════════════════════════════════════════════════════════════════════════════

// find a struct field by name or index, depending on whether it's a tuple
// struct. on failure, emits a diagnostic and returns NULL.
FieldDef *find_struct_field(const StructDef *def, FieldIdent ident,
                            DiagBag *diags, Span span) {
  if (def->is_tuple) {
    int idx = ident.index;
    if (idx < 0 || idx >= def->field_count) {
      diag_error(diags, span,
                 "tuple index %d out of bounds for struct with %d fields", idx,
                 def->field_count);
      return NULL;
    }
    return &def->fields[idx];
  } else {
    StringView name = ident.name;
    for (int i = 0; i < def->field_count; i++) {
      if (sv_equal(def->fields[i].ident.name, name)) {
        return &def->fields[i];
      }
    }

    diag_error(diags, span, "unknown field '" SV_FMT " '", SV_ARG(name));
    return NULL;
  }
}

VariantDef *find_enum_variant(const EnumDef *def, StringView name,
                              DiagBag *diags, Span span) {
  for (int i = 0; i < def->variant_count; i++) {
    if (sv_equal(def->variants[i].name, name)) {
      return &def->variants[i];
    }
  }

  diag_error(diags, span, "unknown variant '" SV_FMT "' in enum '" SV_FMT "'",
             SV_ARG(name), SV_ARG(def->name));
  return NULL;
}

FieldDef *find_variant_field(const VariantDef *def, FieldIdent ident,
                             DiagBag *diags, Span span) {
  if (def->is_tuple) {
    int idx = ident.index;
    if (idx < 0 || idx >= def->field_count) {
      diag_error(diags, span,
                 "tuple index %d out of bounds for variant with %d fields", idx,
                 def->field_count);
      return NULL;
    }
    return &def->fields[idx];
  } else {
    StringView name = ident.name;
    for (int i = 0; i < def->field_count; i++) {
      if (sv_equal(def->fields[i].ident.name, name)) {
        return &def->fields[i];
      }
    }

    diag_error(diags, span,
               "unknown field '" SV_FMT " ' in variant '" SV_FMT "'",
               SV_ARG(name), SV_ARG(def->name));
    return NULL;
  }
}

// ═══════════════════════════════════════════════════════════════════════════════
// Substitution
// ═══════════════════════════════════════════════════════════════════════════════

// build a substitution from two parallel arrays of equal length.
void subst_init(Subst *s, StringView *params, Type **args, int count) {
  *s = (Subst){.params = params, .args = args, .count = count};
}

// recursively replace TY_GENERIC nodes whose .name matches an entry.
// unmatched generics pass through unchanged.
Type *subst_apply(const Subst *s, Type *t, Allocator *al) {
  if (!s->count) {
    return t;
  }

  switch (t->kind) {
  case TY_GENERIC:
    for (int i = 0; i < s->count; i++) {
      if (sv_equal(s->params[i], t->as.generic.name)) {
        return s->args[i];
      }
    }
    assert(false && "subst_apply: generic type not found in substitution");
    return t; // unmatched — pass through

  case TY_INT:
  case TY_FLOAT:
  case TY_BOOL:
  case TY_STRING:
  case TY_UNIT:
  case TY_UNKNOWN:
  case TY_POISON:
    return t;

  case TY_FUNCTION: {
    TypeScratch ps;
    ts_init(&ps, t->as.fun.param_count, al);
    bool changed = false;
    for (int i = 0; i < t->as.fun.param_count; i++) {
      ps.ptr[i] = subst_apply(s, t->as.fun.param_types[i], al);
      changed |= ps.ptr[i] != t->as.fun.param_types[i];
    }
    Type *ret = subst_apply(s, t->as.fun.return_type, al);
    changed |= ret != t->as.fun.return_type;
    return changed ? ty_fun(ps.ptr, ps.count, ret, al) : t;
  }

  case TY_STRUCT: {
    if (!t->as.struc.type_arg_count) {
      return t;
    }
    TypeScratch args;
    ts_init(&args, t->as.struc.type_arg_count, al);
    bool changed = false;
    for (int i = 0; i < t->as.struc.type_arg_count; i++) {
      args.ptr[i] = subst_apply(s, t->as.struc.type_args[i], al);
      changed |= args.ptr[i] != t->as.struc.type_args[i];
    }
    return changed ? ty_struct(t->as.struc.def, args.ptr,
                               t->as.struc.type_arg_count, al)
                   : t;
  }

  case TY_ENUM: {
    if (!t->as.enm.type_arg_count) {
      return t;
    }

    TypeScratch args;
    ts_init(&args, t->as.enm.type_arg_count, al);
    bool changed = false;
    for (int i = 0; i < t->as.enm.type_arg_count; i++) {
      args.ptr[i] = subst_apply(s, t->as.enm.type_args[i], al);
      changed |= args.ptr[i] != t->as.enm.type_args[i];
    }
    return changed
               ? ty_enum(t->as.enm.def, args.ptr, t->as.enm.type_arg_count, al)
               : t;
  }
  default:
    return t;
  }
}

// build a substitution mapping generic type parameters to either fresh unknowns
// or concrete types if provided. the caller is responsible for unifying the
// unknowns
Subst infer_open_generics(InferCtx *ctx, Type **params,
                          Type **concretes /* nullable */, int count, Span span,
                          Allocator *al) {
  if (count == 0) {
    return subst_empty();
  }
  StringView *names = al_alloc(al, sizeof(StringView) * count);
  Type **args = al_alloc(al, sizeof(Type *) * count);
  for (int i = 0; i < count; i++) {
    assert(params[i]->kind == TY_GENERIC);
    names[i] = params[i]->as.generic.name;
    args[i] = concretes && concretes[i]
                  ? concretes[i]
                  : infer_fresh(ctx, params[i]->as.generic.name, NULL,
                                span); // TODO: pass bound from generic.bounds
  }
  Subst s;
  subst_init(&s, names, args, count);
  return s;
}

// ═══════════════════════════════════════════════════════════════════════════════
// Inference
// ═══════════════════════════════════════════════════════════════════════════════

void infer_init(InferCtx *ctx, Allocator *al) {
  ctx->solutions = NULL;
  ctx->nodes = NULL;
  ctx->cap = 0;
  ctx->next_id = 0;
  ctx->al = al;
}

Type *infer_fresh(InferCtx *ctx, StringView param_name, Type *bound,
                  Span intro_span) {
  uint32_t id = ctx->next_id++;

  if ((int)id >= ctx->cap) {
    int new_cap = ctx->cap == 0 ? 4 : ctx->cap * 2;
    ctx->solutions =
        al_realloc(ctx->al, ctx->solutions, sizeof(Type *) * ctx->cap,
                   sizeof(Type *) * new_cap);
    ctx->nodes = al_realloc(ctx->al, ctx->nodes, sizeof(Type *) * ctx->cap,
                            sizeof(Type *) * new_cap);
    memset(ctx->solutions + ctx->cap, 0, sizeof(Type *) * (new_cap - ctx->cap));
    ctx->cap = new_cap;
  }

  Type *t = al_alloc_zero_for(ctx->al, Type);
  t->kind = TY_UNKNOWN;
  t->as.unknown.id = id;
  t->as.unknown.bound = bound;
  t->as.unknown.param_name = param_name;
  t->as.unknown.intro_span = intro_span;

  ctx->solutions[id] = NULL; // free
  ctx->nodes[id] = t;        // keep the node for error reporting

  return t;
}

// walk redirects with path compression.
Type *infer_find(InferCtx *ctx, Type *ty) {
  if (ty->kind != TY_UNKNOWN) {
    return ty;
  }

  Type *slot = ctx->solutions[ty->as.unknown.id];
  if (!slot) {
    return ty; // free — still unknown
  }

  Type *root = infer_find(ctx, slot);
  ctx->solutions[ty->as.unknown.id] = root; // path compress
  return root;
}

bool infer_unify(InferCtx *ctx, Type *a, Type *b, DiagBag *diags, Span span) {
  a = infer_find(ctx, a);
  b = infer_find(ctx, b);

  if (types_equal(a, b)) {
    return true;
  }

  if (a->kind == TY_UNKNOWN) {
    ctx->solutions[a->as.unknown.id] = b;
    return true;
  }
  if (b->kind == TY_UNKNOWN) {
    ctx->solutions[b->as.unknown.id] = a;
    return true;
  }

  if (a->kind != b->kind) {
    char ab[64], bb[64];
    type_sprintf(a, ab, sizeof(ab));
    type_sprintf(b, bb, sizeof(bb));
    diag_error(diags, span, "type mismatch: expected '%s' but got '%s'", ab,
               bb);
    return false;
  }

  switch (a->kind) {
  case TY_INT:
  case TY_FLOAT:
  case TY_BOOL:
  case TY_STRING:
  case TY_UNIT:
    return true; // same kind, same singleton -> equal

  case TY_GENERIC:
    // generics only unify if they're the same node, i.e. the same parameter on
    // the same generic definition. this is a bit stricter than necessary but
    // simplifies inference since we don't have to worry about accidentally
    // unifying two different generic parameters with the same name.
    return false;

  case TY_FUNCTION: {
    TypeFun *af = &a->as.fun, *bf = &b->as.fun;
    if (af->param_count != bf->param_count) {
      diag_error(diags, span, "function arity mismatch");
      return false;
    }
    bool ok = true;
    for (int i = 0; i < af->param_count; i++)
      ok &=
          infer_unify(ctx, af->param_types[i], bf->param_types[i], diags, span);
    return ok & infer_unify(ctx, af->return_type, bf->return_type, diags, span);
  }
  case TY_STRUCT: {
    TypeStruct *as = &a->as.struc, *bs = &b->as.struc;
    if (as->def != bs->def) {
      char ab[64], bb[64];
      type_sprintf(a, ab, sizeof(ab));
      type_sprintf(b, bb, sizeof(bb));
      diag_error(diags, span, "type mismatch: '%s' vs '%s'", ab, bb);
      return false;
    }
    bool ok = true;
    for (int i = 0; i < as->type_arg_count; i++)
      ok &= infer_unify(ctx, as->type_args[i], bs->type_args[i], diags, span);
    return ok;
  }
  case TY_ENUM: {
    TypeEnum *ae = &a->as.enm, *be = &b->as.enm;
    if (ae->def != be->def) {
      char ab[64], bb[64];
      type_sprintf(a, ab, sizeof(ab));
      type_sprintf(b, bb, sizeof(bb));
      diag_error(diags, span, "type mismatch: '%s' vs '%s'", ab, bb);
      return false;
    }
    bool ok = true;
    for (int i = 0; i < ae->type_arg_count; i++)
      ok &= infer_unify(ctx, ae->type_args[i], be->type_args[i], diags, span);
    return ok;
  }
  default:
    assert(false && "infer_unify: unhandled kind");
    return false;
  }
}

// deeply replace solved unknowns, leaving free ones intact.
Type *infer_apply(InferCtx *ctx, Type *ty, Allocator *al) {
  ty = infer_find(ctx, ty);

  switch (ty->kind) {
  case TY_INT:
  case TY_FLOAT:
  case TY_BOOL:
  case TY_STRING:
  case TY_UNIT:
  case TY_UNKNOWN:
  case TY_POISON:
  case TY_GENERIC:
    return ty;

  case TY_FUNCTION: {
    TypeFun *f = &ty->as.fun;
    TypeScratch ps;
    ts_init(&ps, f->param_count, al);
    bool changed = false;

    for (int i = 0; i < f->param_count; i++) {
      ps.ptr[i] = infer_apply(ctx, f->param_types[i], al);
      changed |= ps.ptr[i] != f->param_types[i];
    }
    Type *ret = infer_apply(ctx, f->return_type, al);
    changed |= ret != f->return_type;
    return changed ? ty_fun(ps.ptr, ps.count, ret, al) : ty;
  }

  case TY_STRUCT: {
    TypeStruct *s = &ty->as.struc;
    if (!s->type_arg_count) {
      return ty;
    }
    TypeScratch args;
    ts_init(&args, s->type_arg_count, al);
    bool changed = false;

    for (int i = 0; i < s->type_arg_count; i++) {
      args.ptr[i] = infer_apply(ctx, s->type_args[i], al);
      changed |= args.ptr[i] != s->type_args[i];
    }
    return changed ? ty_struct(s->def, args.ptr, args.count, al) : ty;
  }

  case TY_ENUM: {
    TypeEnum *e = &ty->as.enm;
    if (!e->type_arg_count) {
      return ty;
    }
    TypeScratch args;
    ts_init(&args, e->type_arg_count, al);
    bool changed = false;

    for (int i = 0; i < e->type_arg_count; i++) {
      args.ptr[i] = infer_apply(ctx, e->type_args[i], al);
      changed |= args.ptr[i] != e->type_args[i];
    }
    return changed ? ty_enum(e->def, args.ptr, args.count, al) : ty;
  }

  default:
    return ty;
  }
}

void infer_finalize(InferCtx *ctx, DiagBag *diags) {
  for (uint32_t id = 0; id < ctx->next_id; id++) {
    // Walk to the root solution for this id.
    Type *sol = ctx->solutions[id] ? infer_find(ctx, ctx->solutions[id]) : NULL;

    bool is_free = !sol || sol->kind == TY_UNKNOWN;
    if (!is_free)
      continue;

    Type *node = ctx->nodes[id];
    StringView name = node->as.unknown.param_name;
    Span span = node->as.unknown.intro_span;

    if (name.len > 0) {
      diag_error(diags, span,
                 "cannot infer type for '" SV_FMT "' — add a type annotation",
                 SV_ARG(name));
    } else {
      diag_error(diags, span, "cannot infer type — add a type annotation");
    }
  }
}

// ═══════════════════════════════════════════════════════════════════════════════
// TypeChecker
// ═══════════════════════════════════════════════════════════════════════════════

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

void tc_destroy(TypeChecker *tc) { (void)tc; }

static void tc_register_fun(TypeChecker *tc, Module *m, Decl *decl) {
  assert(decl->kind == DECL_FUN && "expected fun decl");

  DeclFun *fun_decl = &decl->as.fun_decl;
  assert(m->fun_cap > m->fun_count && "fun capacity exceeded");

  FunDef *def = al_alloc_zero_for(tc->al, FunDef);
  // stub
  def->is_pub = decl->is_pub;
  def->name = fun_decl->name;
  def->param_count = fun_decl->param_count;
  def->params = al_alloc_zero(tc->al, sizeof(ParamDef) * fun_decl->param_count);
  def->type_param_count = fun_decl->type_param_count;
  def->type_params =
      al_alloc_zero(tc->al, sizeof(StringView) * fun_decl->type_param_count);

  // register in module
  m->funs[m->fun_count++] = def;
  // set backpointers
  decl->as.fun_decl.def = def;
  def->module = m;
}

static void tc_register_struct(TypeChecker *tc, Module *m, Decl *decl) {
  assert(decl->kind == DECL_STRUCT && "expected struct decl");

  DeclStruct *struct_decl = &decl->as.struct_decl;
  assert(m->struct_cap > m->struct_count && "struct capacity exceeded");

  StructDef *def = al_alloc_zero_for(tc->al, StructDef);
  // stub
  def->is_pub = decl->is_pub;
  def->is_tuple = struct_decl->is_tuple;
  def->name = struct_decl->name;
  def->field_count = struct_decl->field_count;
  def->fields =
      al_alloc_zero(tc->al, sizeof(FieldDef) * struct_decl->field_count);
  def->type_param_count = struct_decl->type_param_count;
  def->type_params =
      al_alloc_zero(tc->al, sizeof(StringView) * struct_decl->type_param_count);

  // register in module
  m->structs[m->struct_count++] = def;
  // set backpointers
  decl->as.struct_decl.def = def;
  def->module = m;
}

static void tc_register_enum(TypeChecker *tc, Module *m, Decl *decl) {
  assert(decl->kind == DECL_ENUM && "expected enum decl");

  DeclEnum *enum_decl = &decl->as.enum_decl;
  assert(m->enum_cap > m->enum_count && "enum capacity exceeded");

  EnumDef *def = al_alloc_zero_for(tc->al, EnumDef);
  // stub
  def->is_pub = decl->is_pub;
  def->name = enum_decl->name;
  def->variant_count = enum_decl->variant_count;
  def->variants =
      al_alloc_zero(tc->al, sizeof(VariantDef) * enum_decl->variant_count);
  def->type_param_count = enum_decl->type_param_count;
  def->type_params =
      al_alloc_zero(tc->al, sizeof(StringView) * enum_decl->type_param_count);

  // register in module
  m->enums[m->enum_count++] = def;
  // set backpointers
  decl->as.enum_decl.def = def;
  def->module = m;
}

void tc_register_module(TypeChecker *tc, Module *m) {
  // counts
  for (int i = 0; i < m->ast->decl_count; i++) {
    switch (m->ast->decls[i]->kind) {
    case DECL_FUN:
      m->fun_cap++;
      break;
    case DECL_STRUCT:
      m->struct_cap++;
      break;
    case DECL_ENUM:
      m->enum_cap++;
      break;
    default:
      assert(false && "unhandled decl kind in tc_register_module");
    }
  }

  // allocate
  m->funs = al_alloc_zero(tc->al, sizeof(FunDef *) * m->fun_cap);
  m->structs = al_alloc_zero(tc->al, sizeof(StructDef *) * m->struct_cap);
  m->enums = al_alloc_zero(tc->al, sizeof(EnumDef *) * m->enum_cap);

  // populate
  for (int i = 0; i < m->ast->decl_count; i++) {
    Decl *decl = m->ast->decls[i];
    switch (decl->kind) {
    case DECL_FUN:
      tc_register_fun(tc, m, decl);
      break;
    case DECL_STRUCT:
      tc_register_struct(tc, m, decl);
      break;
    case DECL_ENUM:
      tc_register_enum(tc, m, decl);
      break;
    default:
      assert(false && "unhandled decl kind in tc_register_module");
    }
  }

  assert(m->fun_count == m->fun_cap && "fun count mismatch after registration");
}

static void resolve_fun_decl(ResolveCtx *rctx, Decl *decl) {
  assert(decl->kind == DECL_FUN && "expected fun decl");
  DeclFun *fun_decl = &decl->as.fun_decl;
  FunDef *fun_def = fun_decl->def;

  for (int i = 0; i < fun_decl->type_param_count; i++) {
    if (fun_decl->type_params[i].inline_bound.refs != NULL) {
      diag_error(rctx->diags, fun_decl->type_params[i].span,
                 "inline bounds on function type parameters are not supported");
    }
    fun_def->type_params[i] =
        ty_generic(fun_decl->type_params[i].name, NULL, 0, rctx->al);
  }

  // begin fun local type scope
  rctx->tyres.tscope = tscope_push(rctx->tyres.tscope, rctx->al);

  for (int i = 0; i < fun_def->type_param_count; i++) {
    tscope_define(rctx->tyres.tscope, fun_decl->type_params[i].name,
                  fun_def->type_params[i], rctx->diags,
                  fun_decl->type_params[i].span, NULL);
  }

  TypeScratch param_types;
  ts_init(&param_types, fun_decl->param_count, rctx->al);
  for (int i = 0; i < fun_decl->param_count; i++) {

    param_types.ptr[i] =
        rctx_resolve(rctx, fun_decl->params[i].type_annotation);

    fun_def->params[i].param_type = param_types.ptr[i];
    fun_def->params[i].name = fun_decl->params[i].name;
  }

  fun_def->return_type = fun_decl->return_type
                             ? rctx_resolve(rctx, fun_decl->return_type)
                             : rctx->tc->t_unit;

  // end fun local type scope
  rctx->tyres.tscope = tscope_pop(rctx->tyres.tscope);

  Type *fun_ty = ty_fun(param_types.ptr, param_types.count,
                        fun_def->return_type, rctx->al);

  fun_def->fun_type = fun_ty;

  VarEntry *ve = NULL;
  vscope_define(&fun_def->module->vscope, fun_def->name, fun_def->fun_type,
                rctx->tc->diags, decl->span, &ve);
  assert(ve && "failed to define function in value scope");
  ve->as.fun = fun_def;

  TypeEntry *te = NULL;
  tscope_define(rctx->tyres.tscope, fun_def->name, fun_ty, rctx->diags,
                decl->span, &te);
  te->as.fun_def = fun_def;
}

static void resolve_struct_decl(ResolveCtx *rctx, Decl *decl) {
  assert(decl->kind == DECL_STRUCT && "expected struct decl");
  DeclStruct *struct_decl = &decl->as.struct_decl;
  StructDef *struct_def = struct_decl->def;

  for (int i = 0; i < struct_decl->type_param_count; i++) {
    if (struct_decl->type_params[i].inline_bound.refs != NULL) {
      diag_error(rctx->diags, struct_decl->type_params[i].span,
                 "inline bounds on struct type parameters are not supported");
    }
    struct_def->type_params[i] =
        ty_generic(struct_decl->type_params[i].name, NULL, 0, rctx->al);
  }

  // begin struct local type scope
  rctx->tyres.tscope = tscope_push(rctx->tyres.tscope, rctx->al);

  for (int i = 0; i < struct_def->type_param_count; i++) {
    tscope_define(rctx->tyres.tscope, struct_decl->type_params[i].name,
                  struct_def->type_params[i], rctx->diags,
                  struct_decl->type_params[i].span, NULL);
  }

  TypeScratch field_types;
  ts_init(&field_types, struct_decl->field_count, rctx->al);
  for (int i = 0; i < struct_decl->field_count; i++) {
    field_types.ptr[i] =
        rctx_resolve(rctx, struct_decl->fields[i].type_annotation);
    struct_def->fields[i].type = field_types.ptr[i];
    if (struct_def->is_tuple) {
      struct_def->fields[i].ident.index = struct_decl->fields[i].ident.index;
    } else {
      struct_def->fields[i].ident.name = struct_decl->fields[i].ident.name;
    }
  }

  // end struct local type scope
  rctx->tyres.tscope = tscope_pop(rctx->tyres.tscope);

  Type *struct_ty = ty_struct(struct_def, struct_def->type_params,
                              struct_def->type_param_count, rctx->al);
  struct_def->self_type = struct_ty;

  TypeEntry *te = NULL;
  tscope_define(rctx->tyres.tscope, struct_def->name, struct_ty, rctx->diags,
                decl->span, &te);
  te->as.struct_def = struct_def;
}

static void resolve_enum_decl(ResolveCtx *rctx, Decl *decl) {
  assert(decl->kind == DECL_ENUM && "expected enum decl");
  DeclEnum *enum_decl = &decl->as.enum_decl;
  EnumDef *enum_def = enum_decl->def;

  for (int i = 0; i < enum_decl->type_param_count; i++) {
    if (enum_decl->type_params[i].inline_bound.refs != NULL) {
      diag_error(rctx->diags, enum_decl->type_params[i].span,
                 "inline bounds on enum type parameters are not supported");
    }
    enum_def->type_params[i] =
        ty_generic(enum_decl->type_params[i].name, NULL, 0, rctx->al);
  }

  // begin type scope
  rctx->tyres.tscope = tscope_push(rctx->tyres.tscope, rctx->al);

  for (int i = 0; i < enum_def->type_param_count; i++) {
    tscope_define(rctx->tyres.tscope, enum_decl->type_params[i].name,
                  enum_def->type_params[i], rctx->diags,
                  enum_decl->type_params[i].span, NULL);
  }

  // resolve variants
  for (int i = 0; i < enum_decl->variant_count; i++) {
    VariantDeclNode *variant_decl = &enum_decl->variants[i];
    VariantDef *variant_def = &enum_def->variants[i];

    variant_def->name = variant_decl->name;
    variant_def->is_tuple = variant_decl->is_tuple;
    variant_def->field_count = variant_decl->field_count;
    variant_def->fields =
        al_alloc_zero(rctx->al, sizeof(FieldDef) * variant_decl->field_count);

    for (int j = 0; j < variant_decl->field_count; j++) {
      variant_def->fields[j].type =
          rctx_resolve(rctx, variant_decl->fields[j].type_annotation);
      if (variant_def->is_tuple) {
        variant_def->fields[j].ident.index =
            variant_decl->fields[j].ident.index;
      } else {
        variant_def->fields[j].ident.name = variant_decl->fields[j].ident.name;
      }
    }
  }

  // end type scope
  rctx->tyres.tscope = tscope_pop(rctx->tyres.tscope);

  Type *enum_ty = ty_enum(enum_def, enum_def->type_params,
                          enum_def->type_param_count, rctx->al);
  enum_def->self_type = enum_ty;

  TypeEntry *te = NULL;
  tscope_define(rctx->tyres.tscope, enum_def->name, enum_ty, rctx->diags,
                decl->span, &te);
  te->as.enum_def = enum_def;
}

static void resolve_decl(ResolveCtx *rctx, Decl *decl) {
  switch (decl->kind) {
  case DECL_FUN:
    resolve_fun_decl(rctx, decl);
    break;
  case DECL_STRUCT:
    resolve_struct_decl(rctx, decl);
    break;
  case DECL_ENUM:
    resolve_enum_decl(rctx, decl);
    break;
  default:
    assert(false && "unhandled decl kind in resolve_decl");
  }
}

bool tc_resolve_module(TypeChecker *tc, Module *m) {
  ResolveCtx rctx;
  rctx_init(&rctx, tc, tc->diags, tc->al);

  for (int i = 0; i < m->ast->decl_count; i++) {
    Decl *decl = m->ast->decls[i];
    rctx.tyres.tscope = &m->tscope;
    resolve_decl(&rctx, decl);
  }

  return true;
}

static Type *resolve_expr(CheckCtx *ctx, Expr *expr, Type *hint);

static void resolve_stmt(CheckCtx *ctx, Stmt *stmt) {
  switch (stmt->kind) {
  case STMT_EXPR: {
    StmtExpr *stmt_expr = &stmt->as.expr_stmt;
    resolve_expr(ctx, stmt_expr->expr, NULL);
    break;
  }
  case STMT_VAR: {
    StmtVar *var = &stmt->as.var_stmt;
    Type *annot_ty = var->type_annotation
                         ? tyres_resolve(&ctx->tyres, var->type_annotation)
                         : NULL;

    Type *init_ty = resolve_expr(ctx, var->initializer, annot_ty);

    if (type_is_poison(init_ty) || (annot_ty && type_is_poison(annot_ty))) {
      init_ty = ctx->tc->t_poison;
    } else if (annot_ty) {
      init_ty =
          infer_unify(&ctx->infer, annot_ty, init_ty, ctx->diags, stmt->span)
              ? annot_ty // prefer annotation on success
              : ctx->tc->t_poison;
    }

    switch (var->binding.kind) {
    case BIND_IDENT: {
      vscope_define(ctx->vscope, var->binding.as.ident, init_ty, ctx->diags,
                    stmt->span, NULL);
      break;
    }
    case BIND_TUPLE: {
      switch (init_ty->kind) {
      case TY_TUPLE: {
        if (!type_is_poison(init_ty) &&
            init_ty->as.tuple.elem_count != var->binding.as.tuple.count) {
          diag_error(
              ctx->diags, stmt->span,
              "tuple destructuring count mismatch: expected %d but got %d",
              var->binding.as.tuple.count, init_ty->as.tuple.elem_count);
          break;
        }

        for (int i = 0; i < var->binding.as.tuple.count; i++) {
          Type *ty = init_ty->as.tuple.elem_types[i];
          vscope_define(ctx->vscope, var->binding.as.tuple.names[i], ty,
                        ctx->diags, stmt->span, NULL);
        }
        break;
      }
      default: {
        char ty_buf[64];
        type_sprintf(init_ty, ty_buf, sizeof(ty_buf));
        diag_error(ctx->diags, stmt->span,
                   "cannot destructure type '%s' as a tuple", ty_buf);
        break;
      }
      }
      break;
    }
    default:
      assert(false && "unhandled var binding kind in resolve_stmt");
    }
    break;
  }
  default:
    assert(false && "unhandled stmt kind in resolve_stmt");
  }
}

static void rewrite_tuple_struct_call(CheckCtx *ctx, Expr *expr,
                                      Type *resolved_struct) {
  assert(expr->kind == EXPR_CALL);
  assert(expr->as.call.callee->kind == EXPR_PATH);

  FieldInit *field_inits =
      al_alloc_zero(ctx->al, sizeof(FieldInit) * expr->as.call.arg_count);
  for (int i = 0; i < expr->as.call.arg_count; i++) {
    field_inits[i].ident.index = i;
    field_inits[i].value = expr->as.call.args[i];
  }

  ExprStructInit init = {
      .path = expr->as.call.callee->as.path_expr.path,
      .field_count = expr->as.call.arg_count,
      .fields = field_inits,
      .resolved_struct = resolved_struct,
  };

  *expr = (Expr){
      .kind = EXPR_STRUCT_INIT,
      .span = expr->span,
      .as.struct_init = init,
  };
}

static void rewrite_tuple_variant_call(CheckCtx *ctx, Expr *expr,
                                       Type *resolved_enum, EnumDef *enum_def,
                                       VariantDef *variant_def) {
  (void)enum_def;
  if (expr->kind == EXPR_PATH) {
    // unit struct/variant constructor with no args
    ExprVariant init = {
        .path = expr->as.path_expr.path,
        .field_count = 0,
        .fields = NULL,
        .resolved_variant = variant_def,
        .resolved_enum = resolved_enum,
    };
    *expr = (Expr){
        .kind = EXPR_VARIANT,
        .span = expr->span,
        .as.variant = init,
    };
    return;
  }

  if (expr->kind == EXPR_CALL) {
    assert(expr->as.call.callee->kind == EXPR_PATH);

    FieldInit *field_inits =
        al_alloc_zero(ctx->al, sizeof(FieldInit) * variant_def->field_count);
    for (int i = 0; i < variant_def->field_count; i++) {
      field_inits[i].ident.index = i;
      field_inits[i].value = expr->as.call.args[i];
    }

    ExprVariant init = {
        .path = expr->as.call.callee->as.path_expr.path,
        .field_count = variant_def->field_count,
        .fields = field_inits,
        .resolved_variant = variant_def,
        .resolved_enum = resolved_enum,
    };

    *expr = (Expr){
        .kind = EXPR_VARIANT,
        .span = expr->span,
        .as.variant = init,
    };
    return;
  }

  if (expr->kind == EXPR_STRUCT_INIT) {
    ExprVariant init = {
        .path = expr->as.struct_init.path,
        .field_count = expr->as.struct_init.field_count,
        .fields = expr->as.struct_init.fields,
        .resolved_variant = variant_def,
        .resolved_enum = resolved_enum,
    };
    *expr = (Expr){
        .kind = EXPR_VARIANT,
        .span = expr->span,
        .as.variant = init,
    };
    return;
  }

  assert(false && "unexpected callee kind in rewrite_tuple_variant_call");
}

static bool resolve_path_segment_args(PathSegment *seg, TypeResolver *tyres,
                                      Allocator *al, TypeScratch *out_args) {
  ts_init(out_args, seg->type_arg_count, al);
  for (int i = 0; i < seg->type_arg_count; i++) {
    out_args->ptr[i] = tyres_resolve(tyres, seg->type_args[i]);
    if (type_is_poison(out_args->ptr[i])) {
      return false;
    }
  }
  return true;
}

static Type *resolve_callee(CheckCtx *ctx, Expr *expr, Subst *subst) {
  assert(expr->kind == EXPR_CALL);
  Expr *callee = expr->as.call.callee;

  if (callee->kind != EXPR_PATH) {
    return resolve_expr(ctx, callee, NULL);
  }

  PathRes r;
  if (!cctx_resolve_path(ctx, &callee->as.path_expr.path, &r)) {
    return ctx->tc->t_poison;
  }

  if (r.kind == PATHRES_VARIANT) {
    if (!r.as.variant.def->is_tuple) {
      char ty_buf[64];
      type_sprintf(r.type, ty_buf, sizeof(ty_buf));
      diag_error(ctx->diags, callee->span,
                 "attempted to call non-tuple enum variant '%s'", ty_buf);
      return ctx->tc->t_poison;
    }
    rewrite_tuple_variant_call(ctx, expr, r.type, r.as.variant.enum_def,
                               r.as.variant.def);
    return r.type;
  }

  switch (r.type->kind) {
  case TY_FUNCTION: {
    FunDef *def = r.as.method.fun;

    if (def->type_param_count == 0) {
      return r.type;
    }

    TypeScratch type_args;
    if (!resolve_path_segment_args(
            &callee->as.path_expr.path
                 .segments[callee->as.path_expr.path.count - 1],
            &ctx->tyres, ctx->al, &type_args)) {
      return ctx->tc->t_poison;
    }

    if (type_args.count > 0 && type_args.count != def->type_param_count) {
      diag_error(ctx->diags, callee->span,
                 "expected %d type arguments but got %d", def->type_param_count,
                 type_args.count);
      return ctx->tc->t_poison;
    }

    *subst = infer_open_generics(&ctx->infer, def->type_params, type_args.ptr,
                                 def->type_param_count, callee->span, ctx->al);

    return subst_apply(subst, r.type, ctx->al);
  }
  case TY_STRUCT: {
    if (!r.type->as.struc.def->is_tuple) {
      char ty_buf[64];
      type_sprintf(r.type, ty_buf, sizeof(ty_buf));
      diag_error(ctx->diags, callee->span,
                 "attempted to call non-tuple struct type '%s'", ty_buf);
      return ctx->tc->t_poison;
    }
    rewrite_tuple_struct_call(ctx, expr, r.type);
    return r.type;
  }
  case TY_ENUM: {
    char ty_buf[64];
    type_sprintf(r.type, ty_buf, sizeof(ty_buf));
    diag_error(ctx->diags, callee->span,
               "attempted to call enum type '%s' without specifying a variant",
               ty_buf);
    return ctx->tc->t_poison;
  }
  default: {
    char ty_buf[64];
    type_sprintf(r.type, ty_buf, sizeof(ty_buf));
    diag_error(ctx->diags, callee->span, "cannot call path of type '%s'",
               ty_buf);
    return ctx->tc->t_poison;
  }
  }
}

static Type *resolve_call_expr(CheckCtx *ctx, Expr *expr, Type *hint) {
  // todo: handle hint

  ExprCall *call = &expr->as.call;

  Subst subst = subst_empty();
  Type *callee_ty = resolve_callee(ctx, expr, &subst);
  if (type_is_poison(callee_ty)) {
    return ctx->tc->t_poison;
  }

  if (expr->kind != EXPR_CALL) {
    return resolve_expr(ctx, expr, hint);
  }

  if (callee_ty->kind != TY_FUNCTION) {
    char callee_buf[64];
    type_sprintf(callee_ty, callee_buf, sizeof(callee_buf));
    diag_error(ctx->diags, call->callee->span,
               "attempted to call non-function type '%s'", callee_buf);
    return ctx->tc->t_poison;
  }

  // check arity
  int expected_argc = callee_ty->as.fun.param_count;
  int got_argc = call->arg_count;
  if (got_argc != expected_argc) {
    diag_error(ctx->diags, expr->span, "expected %d arguments but got %d",
               expected_argc, got_argc);
    return ctx->tc->t_poison;
  }

  bool is_generic = subst.count > 0;

  // check arguments
  bool had_error = false;
  for (int i = 0; i < call->arg_count; i++) {
    Type *param_ty = callee_ty->as.fun.param_types[i];
    Type *arg_ty =
        resolve_expr(ctx, call->args[i], callee_ty->as.fun.param_types[i]);

    if (type_is_poison(arg_ty) || type_is_poison(param_ty)) {
      had_error = true;
      continue;
    }

    if (is_generic) {
      had_error |= !infer_unify(&ctx->infer, arg_ty, param_ty, ctx->diags,
                                call->args[i]->span);
    } else {
      if (!types_equal(arg_ty, param_ty)) {
        char ab[64], pb[64];
        type_sprintf(arg_ty, ab, sizeof(ab));
        type_sprintf(param_ty, pb, sizeof(pb));
        diag_error(ctx->diags, call->args[i]->span,
                   "argument %d: expected '%s' but got '%s'", i + 1, pb, ab);
        had_error = true;
      }
    }
  }

  if (had_error) {
    return ctx->tc->t_poison;
  }

  Type *ret_ty = callee_ty->as.fun.return_type;
  if (is_generic) {
    ret_ty = subst_apply(&subst, ret_ty, ctx->al); // into unsolved unknowns
    ret_ty = infer_apply(&ctx->infer, ret_ty, ctx->al); // apply solved unknowns
  }

  return ret_ty;
}

static bool check_pattern(CheckCtx *ctx, Pattern *pattern, Type *expected_ty);

static bool check_struct_pattern(CheckCtx *ctx, Pattern *pattern,
                                 Type *expected_ty, StructDef *struct_def) {
  if (type_is_poison(expected_ty)) {
    return true;
  }

  if (expected_ty->kind != TY_STRUCT) {
    char et[64];
    type_sprintf(expected_ty, et, sizeof(et));
    diag_error(ctx->diags, pattern->span,
               "expected struct type in struct pattern, got '%s'", et);
    return false;
  }

  if (pattern->as.struc.field_count > 0) {
    if (struct_def->is_tuple &&
        pattern->as.struc.fields[0].ident.name.len > 0) {
      diag_error(
          ctx->diags, pattern->span,
          "matching tuple struct with struct pattern syntax is not allowed");
      return false;
    }

    if (!struct_def->is_tuple &&
        pattern->as.struc.fields[0].ident.name.len == 0) {
      diag_error(ctx->diags, pattern->span,
                 "matching struct with tuple pattern syntax is not allowed");
      return false;
    }
  }

  bool is_generic = struct_def->type_param_count > 0;

  Subst subst = subst_empty();
  if (is_generic) {
    subst = infer_open_generics(
        &ctx->infer, struct_def->type_params, expected_ty->as.struc.type_args,
        struct_def->type_param_count, pattern->span, ctx->al);
  }

  bool sub_pattern_error = false;
  for (int i = 0; i < pattern->as.struc.field_count; i++) {
    FieldPat *fp = &pattern->as.struc.fields[i];
    FieldDef *fd =
        find_struct_field(struct_def, fp->ident, ctx->diags, fp->span);

    Type *field_ty = NULL;
    if (fd == NULL) {
      sub_pattern_error = true;
      field_ty = ctx->tc->t_poison;
    } else {
      field_ty = fd->type;
    }

    if (is_generic) {
      field_ty = subst_apply(&subst, field_ty, ctx->al);
      field_ty = infer_apply(&ctx->infer, field_ty, ctx->al);
    }

    if (fp->sub_pattern != NULL) {
      sub_pattern_error |= check_pattern(ctx, fp->sub_pattern, field_ty);
    } else {
      assert(!struct_def->is_tuple &&
             "tuple struct patterns must have sub-patterns");
      vscope_define(ctx->vscope, fp->ident.name, field_ty, ctx->diags, fp->span,
                    NULL);
    }
  }
  return !sub_pattern_error;
}

static bool check_variant_pattern(CheckCtx *ctx, Pattern *pattern,
                                  Type *expected_ty, EnumDef *enum_def,
                                  VariantDef *variant_def) {
  if (type_is_poison(expected_ty)) {
    return true;
  }

  if (expected_ty->kind != TY_ENUM) {
    char et[64];
    type_sprintf(expected_ty, et, sizeof(et));
    diag_error(ctx->diags, pattern->span,
               "expected enum type in variant pattern, got '%s'", et);
    return false;
  }

  if (expected_ty->kind != TY_ENUM || expected_ty->as.enm.def != enum_def) {
    char et[64], pt[64];
    type_sprintf(expected_ty, et, sizeof(et));
    StringView name =
        pattern->as.variant.path.segments[pattern->as.variant.path.count - 1]
            .name;
    memcpy(pt, name.chars, name.len);
    pt[name.len] = '\0';
    diag_error(ctx->diags, pattern->span,
               "variant '%s' does not belong to expected enum type '%s'", pt,
               et);
    return false;
  }

  if (pattern->as.variant.field_count > 0) {
    if (variant_def->is_tuple &&
        pattern->as.variant.fields[0].ident.name.len != 0) {
      diag_error(
          ctx->diags, pattern->span,
          "matching tuple variant with struct pattern syntax is not allowed");
      return false;
    }

    if (!variant_def->is_tuple &&
        pattern->as.variant.fields[0].ident.name.len == 0) {
      diag_error(
          ctx->diags, pattern->span,
          "matching struct variant with tuple pattern syntax is not allowed");
      return false;
    }
  }

  int expected_argc = variant_def->field_count;
  int got_argc = pattern->as.variant.field_count;
  if (got_argc > 0 && got_argc != expected_argc) {
    char et[64];
    type_sprintf(expected_ty, et, sizeof(et));
    diag_error(ctx->diags, pattern->span,
               "expected %d arguments for variant '%s' but got %d",
               expected_argc, et, got_argc);
    return false;
  }

  bool sub_pattern_error = false;
  int check_n = expected_argc < got_argc ? expected_argc : got_argc;

  Subst subst = subst_empty();
  bool is_generic = enum_def->type_param_count > 0;

  if (enum_def->type_param_count > 0) {
    subst = infer_open_generics(
        &ctx->infer, enum_def->type_params, expected_ty->as.enm.type_args,
        enum_def->type_param_count, pattern->span, ctx->al);
  }

  for (int i = 0; i < check_n; i++) {
    FieldPat *fp = &pattern->as.variant.fields[i];
    FieldDef *fd =
        find_variant_field(variant_def, fp->ident, ctx->diags, fp->span);

    Type *field_ty = NULL;
    if (fd == NULL) {
      sub_pattern_error = true;
      field_ty = ctx->tc->t_poison;
    } else {
      field_ty = fd->type;
    }

    if (is_generic) {
      field_ty = subst_apply(&subst, field_ty, ctx->al);
      field_ty = infer_apply(&ctx->infer, field_ty, ctx->al);
    }

    if (fp->sub_pattern != NULL) {
      sub_pattern_error |= check_pattern(ctx, fp->sub_pattern, field_ty);
    } else {
      assert(!variant_def->is_tuple &&
             "tuple variant patterns must have sub-patterns");
      vscope_define(ctx->vscope, fp->ident.name, field_ty, ctx->diags, fp->span,
                    NULL);
    }
  }

  for (int i = check_n; i < got_argc; i++) {
    sub_pattern_error |= check_pattern(
        ctx, pattern->as.variant.fields[i].sub_pattern, ctx->tc->t_poison);
  }

  return !sub_pattern_error;
}

static bool check_pattern(CheckCtx *ctx, Pattern *pattern, Type *expected_ty) {
  pattern->resolved_type = expected_ty;

  switch (pattern->kind) {
  case PAT_WILDCARD:
    break;
  case PAT_LITERAL: {
    Type *lit_ty = resolve_expr(ctx, pattern->as.literal_expr, NULL);
    if (!type_is_poison(lit_ty) && !type_is_poison(expected_ty)) {
      if (!infer_unify(&ctx->infer, lit_ty, expected_ty, ctx->diags,
                       pattern->span)) {
        return false;
      }
    }
    break;
  }
  case PAT_BIND: {
    vscope_define(ctx->vscope, pattern->as.bind.name, expected_ty, ctx->diags,
                  pattern->span, NULL);
    break;
  }
  case PAT_VARIANT: {
    PathRes r;
    if (!cctx_resolve_path(ctx, &pattern->as.variant.path, &r)) {
      return false;
    }

    if (r.kind == PATHRES_TYPE && r.type->kind == TY_STRUCT) {
      Pattern new = {
          .kind = PAT_STRUCT,
          .span = pattern->span,
          .as.struc =
              {
                  .path = pattern->as.variant.path,
                  .field_count = pattern->as.variant.field_count,
                  .fields = pattern->as.variant.fields,
              },
      };
      *pattern = new;
      return check_struct_pattern(ctx, pattern, expected_ty,
                                  r.type->as.struc.def);
    }

    return check_variant_pattern(ctx, pattern, expected_ty,
                                 r.as.variant.enum_def, r.as.variant.def);
  }
  case PAT_STRUCT: {
    PathRes r;
    if (!cctx_resolve_path(ctx, &pattern->as.struc.path, &r)) {
      return false;
    }

    if (r.kind == PATHRES_VARIANT) {
      Pattern new = {
          .kind = PAT_VARIANT,
          .span = pattern->span,
          .as.variant =
              {
                  .path = pattern->as.struc.path,
                  .field_count = pattern->as.struc.field_count,
                  .fields = pattern->as.struc.fields,
              },
      };
      *pattern = new;
      return check_variant_pattern(ctx, pattern, expected_ty,
                                   r.as.variant.enum_def, r.as.variant.def);
    }

    return check_struct_pattern(ctx, pattern, expected_ty,
                                r.type->as.struc.def);
  }
  case PAT_TUPLE: {
    if (type_is_poison(expected_ty)) {
      break;
    }

    if (expected_ty->kind != TY_TUPLE) {
      char et[64];
      type_sprintf(expected_ty, et, sizeof(et));
      diag_error(ctx->diags, pattern->span,
                 "expected tuple type in tuple pattern, got '%s'", et);
      break;
    }

    int expected_n = expected_ty->as.tuple.elem_count;
    int got_n = pattern->as.tuple.count;

    if (expected_n != got_n) {
      char et[64];
      type_sprintf(expected_ty, et, sizeof(et));
      diag_error(ctx->diags, pattern->span,
                 "expected tuple of size %d in tuple pattern, got %d",
                 expected_n, got_n);
      break;
    }

    int check_n = expected_n < got_n ? expected_n : got_n;
    bool sub_pattern_error = false;
    for (int i = 0; i < check_n; i++) {
      Type *elem_ty = expected_ty->as.tuple.elem_types[i];
      Pattern *elem_pat = pattern->as.tuple.elems[i];
      sub_pattern_error |= check_pattern(ctx, elem_pat, elem_ty);
    }

    if (sub_pattern_error) {
      return false;
    }
    break;
  }
  }

  return true;
}

static Type *resolve_match_expr(CheckCtx *ctx, Expr *expr, Type *hint) {
  Type *subject_ty = resolve_expr(ctx, expr->as.match.subject, NULL);
  Type *result_ty = hint;

  bool had_error = false;
  for (int i = 0; i < expr->as.match.arm_count; i++) {
    MatchArm *arm = &expr->as.match.arms[i];

    ctx->vscope = vscope_push(ctx->vscope, false, false, ctx->al);

    had_error |= !check_pattern(ctx, arm->pattern, subject_ty);

    if (arm->guard != NULL) {
      Type *guard_ty = resolve_expr(ctx, arm->guard, NULL);
      if (!type_is_poison(guard_ty) && guard_ty->kind != TY_BOOL) {
        char gt[64];
        type_sprintf(guard_ty, gt, sizeof(gt));
        diag_error(ctx->diags, arm->guard->span,
                   "match guard must be of type Bool, got '%s'", gt);
        had_error = true;
      }
    }

    Type *arm_ty = resolve_expr(ctx, arm->body, NULL);

    if (result_ty == NULL) {
      result_ty = arm_ty;
    } else if (!type_is_poison(result_ty) && !type_is_poison(arm_ty)) {
      if (!infer_unify(&ctx->infer, result_ty, arm_ty, ctx->diags,
                       arm->body->span)) {
        had_error = true;
      }
    } else if (type_is_poison(result_ty)) {
      result_ty = arm_ty; // recover
    }

    ctx->vscope = vscope_pop(ctx->vscope);
  }

  if (had_error) {
    return ctx->tc->t_poison;
  }

  return result_ty ? result_ty : ctx->tc->t_unit;
}

static Type *resolve_expr(CheckCtx *ctx, Expr *expr, Type *hint) {
  (void)hint;
  (void)ctx;

  Type *result = NULL;
  assert(expr && "expr is null");

  switch (expr->kind) {
  case EXPR_INT:
    result = ctx->tc->t_int;
    break;
  case EXPR_FLOAT:
    result = ctx->tc->t_float;
    break;
  case EXPR_BOOL:
    result = ctx->tc->t_bool;
    break;
  case EXPR_STRING:
    result = ctx->tc->t_string;
    break;
  case EXPR_BLOCK: {
    ExprBlock *block = &expr->as.block;
    ctx->vscope = vscope_push(ctx->vscope, false, false, ctx->al);

    for (int i = 0; i < block->stmt_count; i++) {
      resolve_stmt(ctx, block->stmts[i]);
    }

    if (block->tail_expr != NULL) {
      result = resolve_expr(ctx, block->tail_expr, NULL);
    } else {
      result = ctx->tc->t_unit;
    }

    ctx->vscope = vscope_pop(ctx->vscope);
    break;
  }
  case EXPR_BINARY: {
    ExprBinary *binary = &expr->as.binary;
    Type *lhs = resolve_expr(ctx, binary->left, NULL);
    Type *rhs = resolve_expr(ctx, binary->right, NULL);

    if (type_is_poison(lhs) || type_is_poison(rhs)) {
      return ctx->tc->t_poison;
    }

    if (lhs->kind == TY_GENERIC && rhs->kind == TY_GENERIC) {
      // todo:
      return ctx->tc->t_poison;
    }

    TokenType op = binary->op;
    bool is_arith =
        (op == TOKEN_PLUS || op == TOKEN_MINUS || op == TOKEN_STAR ||
         op == TOKEN_SLASH || op == TOKEN_PERCENT);
    bool is_cmp = (op == TOKEN_LT || op == TOKEN_LTEQ || op == TOKEN_GT ||
                   op == TOKEN_GTEQ);
    bool is_eq = (op == TOKEN_EQEQ || op == TOKEN_BANGEQ);
    bool is_logic = (op == TOKEN_AND || op == TOKEN_OR);

    if (is_arith) {
      if (!type_is_numeric(lhs) || !type_is_numeric(rhs)) {
        char lhs_buf[64], rhs_buf[64];
        type_sprintf(lhs, lhs_buf, sizeof(lhs_buf));
        type_sprintf(rhs, rhs_buf, sizeof(rhs_buf));
        diag_error(
            ctx->diags, expr->span,
            "arithmetic operator requires numeric types, got '%s' and '%s'",
            lhs_buf, rhs_buf);
        result = ty_poison();
      } else {
        // Int op Float widens to Float.
        result = (lhs->kind == TY_FLOAT || rhs->kind == TY_FLOAT) ? ty_float()
                                                                  : ty_int();
      }
    } else if (is_cmp) {
      if (!type_is_numeric(lhs) || !type_is_numeric(rhs)) {
        char lhs_buf[64], rhs_buf[64];
        type_sprintf(lhs, lhs_buf, sizeof(lhs_buf));
        type_sprintf(rhs, rhs_buf, sizeof(rhs_buf));
        diag_error(ctx->diags, expr->span,
                   "comparison requires numeric types, got '%s' and '%s'",
                   lhs_buf, rhs_buf);
        result = ty_poison();
      } else {
        result = ty_bool();
      }
    } else if (is_eq) {
      if (!types_equal(lhs, rhs)) {
        char lhs_buf[64], rhs_buf[64];
        type_sprintf(lhs, lhs_buf, sizeof(lhs_buf));
        type_sprintf(rhs, rhs_buf, sizeof(rhs_buf));
        diag_error(ctx->diags, expr->span,
                   "cannot compare '%s' and '%s' with == / !=", lhs_buf,
                   rhs_buf);
        result = ty_poison();
      } else {
        result = ty_bool();
      }
    } else if (is_logic) {
      if (lhs->kind != TY_BOOL || rhs->kind != TY_BOOL) {
        diag_error(ctx->diags, expr->span,
                   "'%s' requires Bool operands, got '%s' and '%s'",
                   op == TOKEN_AND ? "and" : "or", type_name(lhs),
                   type_name(rhs));
        result = ty_poison();
      } else {
        result = ty_bool();
      }
    } else {
      if (op == TOKEN_PLUS && lhs->kind == TY_STRING &&
          rhs->kind == TY_STRING) {
        result = ty_string();
      } else {
        char lhs_buf[64], rhs_buf[64];
        type_sprintf(lhs, lhs_buf, sizeof(lhs_buf));
        type_sprintf(rhs, rhs_buf, sizeof(rhs_buf));
        diag_error(ctx->diags, expr->span,
                   "unsupported operator for types '%s' and '%s'", lhs_buf,
                   rhs_buf);
        result = ctx->tc->t_poison;
      }
    }
    break;
  }
  case EXPR_CALL: {
    result = resolve_call_expr(ctx, expr, hint);
    break;
  }
  case EXPR_PATH: {
    if (expr->as.path_expr.path.count != 1) {
      diag_error(ctx->diags, expr->span,
                 "unexpected multi-segment path expression without a call");
      result = ctx->tc->t_poison;
      break;
    }

    StringView name =
        expr->as.path_expr.path.segments[expr->as.path_expr.path.count - 1]
            .name;

    VarEntry *ve = vscope_lookup(ctx->vscope, name, NULL);
    if (!ve) {
      diag_error(ctx->diags, expr->span, "undefined variable '%.*s'", name.len,
                 name.chars);
      result = ctx->tc->t_poison;
      break;
    }

    Type *ty = ve->type;

    if (ty->kind == TY_FUNCTION) {
      FunDef *def = ve->as.fun;

      if (def == NULL || def->type_param_count == 0) {
        return ty;
      }

      TypeScratch type_args;
      if (!resolve_path_segment_args(
              &expr->as.path_expr.path
                   .segments[expr->as.path_expr.path.count - 1],
              &ctx->tyres, ctx->al, &type_args)) {
        return ctx->tc->t_poison;
      }

      if (type_args.count > 0 && type_args.count != def->type_param_count) {
        diag_error(ctx->diags, expr->span,
                   "expected %d type arguments but got %d",
                   def->type_param_count, type_args.count);
        return ctx->tc->t_poison;
      }

      Subst subst =
          infer_open_generics(&ctx->infer, def->type_params, type_args.ptr,
                              def->type_param_count, expr->span, ctx->al);

      ty = subst_apply(&subst, ty, ctx->al);
      ty = infer_apply(&ctx->infer, ty, ctx->al);

      result = ty;
      break;
    }

    result = ve->type;
    break;
  }
  case EXPR_STRUCT_INIT: {
    ExprStructInit *init = &expr->as.struct_init;

    if (!init->resolved_struct) {
      PathRes r;
      if (!cctx_resolve_path(ctx, &init->path, &r)) {
        result = ctx->tc->t_poison;
        break;
      }

      if (r.kind == PATHRES_VARIANT) {
        rewrite_tuple_variant_call(ctx, expr, r.type, r.as.variant.enum_def,
                                   r.as.variant.def);
        return resolve_expr(ctx, expr,
                            hint); // re-resolve as variant initializer
      }

      if (r.type->kind != TY_STRUCT) {
        char ty_buf[64];
        type_sprintf(r.type, ty_buf, sizeof(ty_buf));
        diag_error(ctx->diags, expr->span,
                   "attempted to initialize non-struct type '%s'", ty_buf);
        result = ctx->tc->t_poison;
        break;
      }

      init->resolved_struct = r.type;
    }

    Type *struct_ty = init->resolved_struct;
    StructDef *def = struct_ty->as.struc.def;

    // check arity
    int expected_field_count = def->field_count;
    int got_field_count = init->field_count;
    if (got_field_count != expected_field_count) {
      diag_error(ctx->diags, expr->span,
                 "expected %d fields but got %d in struct initializer",
                 expected_field_count, got_field_count);
      result = ctx->tc->t_poison;
      break;
    }

    Subst subst = subst_empty();
    bool is_generic = def->type_param_count > 0;

    if (is_generic) {
      bool had_explicit_args = struct_ty != def->self_type;
      Type **args = had_explicit_args ? struct_ty->as.struc.type_args : NULL;
      subst = infer_open_generics(&ctx->infer, def->type_params, args,
                                  def->type_param_count, expr->span, ctx->al);
    }

    // check fields
    bool had_error = false;
    for (int i = 0; i < init->field_count; i++) {
      FieldInit *field_init = &init->fields[i];

      FieldDef *field_def =
          find_struct_field(def, field_init->ident, ctx->diags, expr->span);
      if (!field_def) {
        had_error = true;
        continue;
      }

      Type *field_ty = field_def->type;
      if (is_generic) {
        field_ty = subst_apply(&subst, field_ty, ctx->al);
      }

      Type *arg_ty = resolve_expr(ctx, field_init->value, field_ty);
      if (type_is_poison(arg_ty) || type_is_poison(field_def->type)) {
        had_error = true;
        continue;
      }

      if (!infer_unify(&ctx->infer, arg_ty, field_ty, ctx->diags,
                       field_init->value->span)) {
        had_error = true;
      }
    }

    if (had_error) {
      result = ctx->tc->t_poison;
      break;
    }

    if (!is_generic) {
      result = def->self_type;
      break;
    }

    result = subst_apply(&subst, def->self_type, ctx->al);
    result = infer_apply(&ctx->infer, result, ctx->al);

    break;
  }
  case EXPR_VARIANT: {
    ExprVariant *init = &expr->as.variant;
    Type *resolved_enum_ty = init->resolved_enum;
    EnumDef *enum_def = resolved_enum_ty->as.enm.def;
    VariantDef *variant_def = init->resolved_variant;

    // check arity
    int expected_field_count = variant_def->field_count;
    int got_field_count = init->field_count;
    if (got_field_count != expected_field_count) {
      diag_error(ctx->diags, expr->span,
                 "expected %d fields but got %d in enum variant initializer",
                 expected_field_count, got_field_count);
      result = ctx->tc->t_poison;
      break;
    }

    Subst subst = subst_empty();
    bool is_generic = enum_def->type_param_count > 0;

    if (is_generic) {
      bool had_explicit_args = resolved_enum_ty != enum_def->self_type;
      Type **args =
          had_explicit_args ? resolved_enum_ty->as.enm.type_args : NULL;
      subst =
          infer_open_generics(&ctx->infer, enum_def->type_params, args,
                              enum_def->type_param_count, expr->span, ctx->al);
    }

    // check fields
    bool had_error = false;
    for (int i = 0; i < init->field_count; i++) {
      FieldInit *field_init = &init->fields[i];

      FieldDef *field_def = &variant_def->fields[i];
      Type *field_ty = field_def->type;
      if (is_generic) {
        field_ty = subst_apply(&subst, field_ty, ctx->al);
      }

      Type *arg_ty = resolve_expr(ctx, field_init->value, field_ty);
      if (type_is_poison(arg_ty) || type_is_poison(field_def->type)) {
        had_error = true;
        continue;
      }

      if (!infer_unify(&ctx->infer, arg_ty, field_ty, ctx->diags,
                       field_init->value->span)) {
        had_error = true;
      }
    }

    if (had_error) {
      result = ctx->tc->t_poison;
      break;
    }

    if (!is_generic) {
      result = enum_def->self_type;
      break;
    }

    result = subst_apply(&subst, enum_def->self_type, ctx->al);
    result = infer_apply(&ctx->infer, result, ctx->al);
    break;
  }
  case EXPR_FIELD: {
    ExprField *field = &expr->as.field;
    Type *base_ty = resolve_expr(ctx, field->object, NULL);

    if (type_is_poison(base_ty)) {
      result = ctx->tc->t_poison;
      break;
    }

    if (base_ty->kind == TY_STRUCT) {
      StructDef *def = base_ty->as.struc.def;

      if (field->is_tuple != def->is_tuple) {
        diag_error(ctx->diags, expr->span,
                   "cannot access tuple field with '.' syntax or vice versa");
        result = ctx->tc->t_poison;
        break;
      }

      Subst subst = subst_empty();
      bool is_generic = def->type_param_count > 0;
      if (is_generic) {
        subst = infer_open_generics(&ctx->infer, def->type_params,
                                    base_ty->as.struc.type_args,
                                    def->type_param_count, expr->span, ctx->al);
      }

      FieldDef *field_def =
          find_struct_field(def, field->ident, ctx->diags, expr->span);
      if (!field_def) {
        result = ctx->tc->t_poison;
      } else {
        result = field_def->type;
      }

      if (is_generic) {
        result = subst_apply(&subst, result, ctx->al);
        // result = infer_apply(&ctx->infer, result, ctx->al);
      }

      break;
    }

    char base_buf[64];
    type_sprintf(base_ty, base_buf, sizeof(base_buf));
    diag_error(ctx->diags, expr->span,
               "field access is not supported on type '%s'", base_buf);
    result = ctx->tc->t_poison;
    break;
  }
  case EXPR_ASSIGN: {
    ExprAssign *assign = &expr->as.assign;
    Type *lhs_ty = resolve_expr(ctx, assign->target, NULL);
    Type *rhs_ty = resolve_expr(ctx, assign->value, lhs_ty);

    if (type_is_poison(lhs_ty) || type_is_poison(rhs_ty)) {
      result = ctx->tc->t_poison;
    } else if (!infer_unify(&ctx->infer, lhs_ty, rhs_ty, ctx->diags,
                            expr->span)) {
      result = ctx->tc->t_poison;
    } else {
      result = lhs_ty;
    }
    break;
  }
  case EXPR_MATCH:
    result = resolve_match_expr(ctx, expr, hint);
    break;
  case EXPR_TUPLE: {
    ExprTuple *tuple = &expr->as.tuple;

    TypeScratch elem_types;
    ts_init(&elem_types, tuple->count, ctx->al);

    bool had_error = false;
    for (int i = 0; i < tuple->count; i++) {
      elem_types.ptr[i] = resolve_expr(ctx, tuple->elems[i], NULL);
      if (type_is_poison(elem_types.ptr[i])) {
        had_error = true;
      }
    }
    if (had_error) {
      result = ctx->tc->t_poison;
    } else {
      result = ty_tuple(elem_types.ptr, tuple->count, ctx->al);
    }
    break;
  }
  default:
    assert(false && "unhandled expr kind in resolve_expr");
  }

  assert(result && "result is null");
  return result;
}

static Type *resolve_expr_coerced(CheckCtx *ctx, Expr *expr, Type *expected) {
  Type *actual = resolve_expr(ctx, expr, expected);
  if (type_is_poison(actual) || type_is_poison(expected)) {
    return ctx->tc->t_poison;
  }
  if (!infer_unify(&ctx->infer, actual, expected, ctx->diags, expr->span)) {
    return ctx->tc->t_poison;
  }
  return actual;
}

static void tc_check_fun(TypeChecker *tc, Decl *decl) {
  assert(decl->kind == DECL_FUN && "expected fun decl");
  DeclFun *fun_decl = &decl->as.fun_decl;
  FunDef *fun_def = fun_decl->def;

  CheckCtx cctx;
  cctx_init(&cctx, tc, fun_def->module, tc->diags, tc->al);

  cctx.fun = fun_def;
  cctx.return_type = fun_def->return_type;

  // begin type scope
  cctx.tyres.tscope = tscope_push(cctx.tyres.tscope, cctx.al);

  for (int i = 0; i < fun_def->type_param_count; i++) {
    tscope_define(cctx.tyres.tscope, fun_decl->type_params[i].name,
                  fun_def->type_params[i], cctx.diags,
                  fun_decl->type_params[i].span, NULL);
  }

  // begin var scope
  cctx.vscope = vscope_push(cctx.vscope, true, false, cctx.al);
  for (int i = 0; i < fun_def->param_count; i++) {
    vscope_define(cctx.vscope, fun_def->params[i].name,
                  fun_def->params[i].param_type, cctx.diags, (Span){0}, NULL);
  }

  resolve_expr_coerced(&cctx, fun_decl->body, fun_def->return_type);

  // end var scope
  cctx.vscope = vscope_pop(cctx.vscope);

  // end type scope
  cctx.tyres.tscope = tscope_pop(cctx.tyres.tscope);

  infer_finalize(&cctx.infer, cctx.diags);
}

bool tc_check_module(TypeChecker *tc, Module *m) {
  for (int i = 0; i < m->ast->decl_count; i++) {
    Decl *decl = m->ast->decls[i];
    switch (decl->kind) {
    case DECL_FUN:
      tc_check_fun(tc, decl);
      break;
    case DECL_STRUCT:
    case DECL_ENUM:
      break;
    default:
      assert(false && "unhandled decl kind in tc_check_module");
    }
  }

  return true;
}

// ═══════════════════════════════════════════════════════════════════════════════
// ValueScope
// ═══════════════════════════════════════════════════════════════════════════════

void vscope_init(ValueScope *scope, ValueScope *parent, Allocator *al) {
  scope->entries = NULL;
  scope->count = 0;
  scope->cap = 0;

  scope->parent = parent;
  scope->is_fn_boundary = false;
  scope->is_loop = false;
  scope->next_slot = parent ? parent->next_slot : 0;

  scope->al = al;
}

// push a new scope. next_slot is inherited from parent unless is_fn_boundary.
ValueScope *vscope_push(ValueScope *parent, bool is_fn_boundary, bool is_loop,
                        Allocator *al) {
  ValueScope *scope = al_alloc_zero_for(al, ValueScope);
  vscope_init(scope, parent, al);
  scope->is_fn_boundary = is_fn_boundary;
  scope->is_loop = is_loop;
  return scope;
}

// pop this scope, returning its parent. does not free (arena-allocated).
ValueScope *vscope_pop(ValueScope *scope) {
  assert(scope->parent && "cannot pop root scope");
  return scope->parent;
}

// walk the parent chain. sets *out_crossed_fn if a fn boundary was crossed.
// returns null if not found.
VarEntry *vscope_lookup(ValueScope *scope, StringView name,
                        bool *out_crossed_fn) {
  bool crossed = false;
  for (ValueScope *s = scope; s; s = s->parent) {
    for (int i = 0; i < s->count; i++) {
      if (sv_equal(s->entries[i].name, name)) {
        if (out_crossed_fn) {
          *out_crossed_fn = crossed;
        }
        return &s->entries[i];
      }
    }
    // we exhausted this scope without a hit; if it was a fn boundary,
    // anything found beyond here is an upvalue
    if (s->is_fn_boundary) {
      crossed = true;
    }
  }
  return NULL;
}

int vscope_define(ValueScope *scope, StringView name, Type *type,
                  DiagBag *diags, Span span, VarEntry **ref) {
  // todo: check for duplicates or shadowing and emit diags
  (void)diags;
  (void)span;

  if (scope->count >= scope->cap) {
    int new_cap = scope->cap == 0 ? 4 : scope->cap * 2;
    scope->entries =
        al_realloc(scope->al, scope->entries, sizeof(VarEntry) * scope->cap,
                   sizeof(VarEntry) * new_cap);
    assert(scope->entries && "out of memory");
    scope->cap = new_cap;
  }

  int slot = scope->next_slot++;
  scope->entries[scope->count++] = (VarEntry){
      .name = name,
      .type = type,
      .slot = slot,
      .is_captured = false,
  };
  if (ref) {
    *ref = &scope->entries[scope->count - 1];
  }
  return slot;
}

// ═══════════════════════════════════════════════════════════════════════════════
// TypeScope
// ═══════════════════════════════════════════════════════════════════════════════

void tscope_init(TypeScope *scope, TypeScope *parent, Allocator *al) {
  scope->entries = NULL;
  scope->count = 0;
  scope->cap = 0;

  scope->parent = parent;
  scope->al = al;
}

TypeScope *tscope_push(TypeScope *parent, Allocator *al) {
  TypeScope *scope = al_alloc_zero_for(al, TypeScope);
  tscope_init(scope, parent, al);
  return scope;
}

TypeScope *tscope_pop(TypeScope *scope) { return scope->parent; }

// walk parent chain; return null if not found.
TypeEntry *tscope_lookup(TypeScope *scope, StringView name) {
  for (TypeScope *s = scope; s; s = s->parent) {
    for (int i = 0; i < s->count; i++) {
      if (sv_equal(s->entries[i].name, name)) {
        return &s->entries[i];
      }
    }
  }
  return NULL;
}

// define in the current (top) scope.
// emits a diagnostic if the name is already defined in this exact scope.
void tscope_define(TypeScope *scope, StringView name, Type *type,
                   DiagBag *diags, Span span, TypeEntry **ref /*nullable*/) {
  // optional: check for shadowing in current scope only
  (void)diags;
  (void)span;

  if (scope->count >= scope->cap) {
    int new_cap = scope->cap == 0 ? 4 : scope->cap * 2;
    scope->entries =
        al_realloc(scope->al, scope->entries, sizeof(TypeEntry) * scope->cap,
                   sizeof(TypeEntry) * new_cap);
    scope->cap = new_cap;
  }
  scope->entries[scope->count++] = (TypeEntry){.name = name, .type = type};
  if (ref) {
    *ref = &scope->entries[scope->count - 1];
  }
}

// ═══════════════════════════════════════════════════════════════════════════════
// TypeResolver
// ═══════════════════════════════════════════════════════════════════════════════

Type *tyres_resolve(TypeResolver *r, TypeNode *node) {
  if (node->resolved) {
    return node->resolved;
  }

  Type *result = NULL;

  switch (node->kind) {
  case TYNODE_UNIT:
    result = r->tc->t_unit;
    break;
  case TYNODE_NAMED: {
    TypeNodeNamed *named = &node->as.named;

    if (named->path.count == 1 && named->path.segments[0].type_arg_count == 0) {
      StringView name = named->path.segments[0].name;

      if (sv_equal_cstr(name, "Int")) {
        result = r->tc->t_int;
        break;
      } else if (sv_equal_cstr(name, "Float")) {
        result = r->tc->t_float;
        break;
      } else if (sv_equal_cstr(name, "Bool")) {
        result = r->tc->t_bool;
        break;
      } else if (sv_equal_cstr(name, "String")) {
        result = r->tc->t_string;
        break;
      } else if (sv_equal_cstr(name, "Unit")) {
        result = r->tc->t_unit;
        break;
      } else if (sv_equal_cstr(name, "_")) {
        if (!r->infer) {
          diag_error(r->diags, node->span,
                     "'_' is not allowed in this context");
          result = r->tc->t_poison;
          break;
        }
        result = infer_fresh(r->infer, (StringView){0}, NULL, node->span);
        break;
      }

      TypeEntry *e = tscope_lookup(r->tscope, name);
      if (e) {
        result = e->type;
        break;
      }

      diag_error(r->tc->diags, node->span, "unknown type: " SV_FMT,
                 SV_ARG(name));
      result = r->tc->t_poison;
      break;
    }

    PathRes pr;
    PathResCtx ctx = {
        .path = &named->path,
        // .vscope = NULL,
        .tscope = r->tscope,
        .tyres = r,
        .diags = r->diags,
        .al = r->al,
    };
    if (!resolve_path(&ctx, &pr)) {
      diag_error(r->tc->diags, node->span, "unresolved type");
      result = r->tc->t_poison;
    } else {
      result = pr.type;
    }

    break;
  }
  case TYNODE_TUPLE: {
    TypeNodeTuple *tuple = &node->as.tuple;
    TypeScratch elem_types;
    ts_init(&elem_types, tuple->count, r->al);
    for (int i = 0; i < tuple->count; i++) {
      elem_types.ptr[i] = tyres_resolve(r, tuple->elems[i]);
    }
    result = ty_tuple(elem_types.ptr, tuple->count, r->al);
    break;
  }
  case TYNODE_FUN: {
    TypeScratch param_types;
    ts_init(&param_types, node->as.fun.param_count, r->al);
    for (int i = 0; i < node->as.fun.param_count; i++) {
      param_types.ptr[i] = tyres_resolve(r, node->as.fun.param_types[i]);
    }
    Type *return_type = tyres_resolve(r, node->as.fun.return_type);
    result =
        ty_fun(param_types.ptr, node->as.fun.param_count, return_type, r->al);
    break;
  }
  default:
    assert(false && "unhandled type node kind in tyres_resolve");
    break;
  }

  assert(result != NULL && "tyres_resolve failed to resolve a type node");
  node->resolved = result;
  return result;
}

// ═══════════════════════════════════════════════════════════════════════════════
// ResolveCtx
// ═══════════════════════════════════════════════════════════════════════════════

void rctx_init(ResolveCtx *rctx, TypeChecker *tc, DiagBag *diags,
               Allocator *al) {
  memset(rctx, 0, sizeof(*rctx));

  rctx->tc = tc;
  rctx->diags = diags;
  rctx->al = al;

  rctx->tyres = (TypeResolver){
      .tc = tc,
      .tscope = NULL,
      .diags = diags,
      .al = al,
  };
}

// ═══════════════════════════════════════════════════════════════════════════════
// CheckCtx
// ═══════════════════════════════════════════════════════════════════════════════

void cctx_init(CheckCtx *cctx, TypeChecker *tc, Module *m, DiagBag *diags,
               Allocator *al) {
  memset(cctx, 0, sizeof(*cctx));
  infer_init(&cctx->infer, al);

  cctx->tc = tc;
  cctx->diags = diags;
  cctx->al = al;

  cctx->tyres = (TypeResolver){
      .tc = tc,
      .tscope = NULL,
      .diags = diags,
      .infer = &cctx->infer,
      .al = al,
  };

  cctx->vscope = &m->vscope;
  cctx->tscope = &m->tscope;

  cctx->tyres.tscope = cctx->tscope;
}

typedef enum {
  PATHRES_CTX_SCOPE,
  PATHRES_CTX_STRUCT,
  PATHRES_CTX_ENUM,
} PathResCtxKind;

typedef struct {
  PathResCtxKind kind;
  union {
    struct {
      StructDef *def;
      Type *inst;
    } struct_;
    struct {
      EnumDef *def;
      Type *inst;
    } enum_;
  } scope;
} PathResCtx_;

bool resolve_path(PathResCtx *ctx, PathRes *out_res) {
  assert(out_res && "out_res is null");
  PathResCtx_ res_ctx = {.kind = PATHRES_CTX_SCOPE};
  memset(out_res, 0, sizeof(*out_res));
  Path *path = ctx->path;

  for (int i = 0; i < path->count; i++) {
    StringView segment = path->segments[i].name;
    bool is_last = (i == path->count - 1);

    switch (res_ctx.kind) {
    case PATHRES_CTX_SCOPE: {
      TypeEntry *te = tscope_lookup(ctx->tscope, segment);
      if (!te) {
        diag_error(ctx->diags, path->span, "unknown type '" SV_FMT "' in path",
                   SV_ARG(segment));
        return false;
      }
      switch (te->type->kind) {
      case TY_FUNCTION: {
        if (!is_last) {
          diag_error(ctx->diags, path->span,
                     "cannot access member '" SV_FMT "' of function type",
                     SV_ARG(segment));
          return false;
        }
        *out_res = (PathRes){
            .kind = PATHRES_METHOD,
            .type = te->type,
            .as.method.fun = te->as.fun_def,
        };
        return true;
      }
      case TY_STRUCT: {
        StructDef *def = te->as.struct_def;

        TypeScratch type_args;
        if (!resolve_path_segment_args(&path->segments[i], ctx->tyres, ctx->al,
                                       &type_args)) {
          return false;
        }

        if (type_args.count > 0 && type_args.count != def->type_param_count) {
          diag_error(ctx->diags, path->span,
                     "expected %d type arguments but got %d for struct '" SV_FMT
                     "'",
                     def->type_param_count, type_args.count, SV_ARG(def->name));
          return false;
        }

        Type *struct_ty =
            (type_args.count > 0)
                ? ty_struct(def, type_args.ptr, type_args.count, ctx->al)
                : def->self_type;

        if (is_last) {
          *out_res = (PathRes){
              .kind = PATHRES_TYPE,
              .type = struct_ty,
          };
          return true;
        }

        res_ctx = (PathResCtx_){
            .kind = PATHRES_CTX_STRUCT,
            .scope.struct_ =
                {
                    .def = def,
                    .inst = struct_ty,
                },
        };
        break;
      }
      case TY_ENUM: {
        EnumDef *def = te->as.enum_def;

        TypeScratch type_args;
        if (!resolve_path_segment_args(&path->segments[i], ctx->tyres, ctx->al,
                                       &type_args)) {
          return false;
        }

        if (type_args.count > 0 && type_args.count != def->type_param_count) {
          diag_error(ctx->diags, path->span,
                     "expected %d type arguments but got %d for enum '" SV_FMT
                     "'",
                     def->type_param_count, type_args.count, SV_ARG(def->name));
          return false;
        }

        Type *enum_ty =
            (type_args.count > 0)
                ? ty_enum(def, type_args.ptr, type_args.count, ctx->al)
                : def->self_type;

        if (is_last) {
          *out_res = (PathRes){
              .kind = PATHRES_TYPE,
              .type = enum_ty,
          };
          return true;
        }

        res_ctx = (PathResCtx_){
            .kind = PATHRES_CTX_ENUM,
            .scope.enum_ =
                {
                    .def = def,
                    .inst = enum_ty,
                },
        };
        break;
      }
      default:
        assert(false &&
               "unhandled type kind in cctx_resolve_path PATHRES_SCOPE");
        return false;
      }
      break;
    }
    case PATHRES_CTX_ENUM: {
      EnumDef *def = res_ctx.scope.enum_.def;
      Type *enum_ty = res_ctx.scope.enum_.inst;

      VariantDef *variant =
          find_enum_variant(def, segment, ctx->diags, path->span);
      if (!variant) {
        return false;
      }

      if (!is_last) {
        diag_error(ctx->diags, path->span,
                   "cannot access member '" SV_FMT "' of enum variant",
                   SV_ARG(segment));
        return false;
      }

      if (path->segments[i].type_arg_count > 0) {
        diag_error(ctx->diags, path->span,
                   "cannot apply type arguments to enum variant '" SV_FMT
                   "'. did you mean to apply them to the enum '" SV_FMT
                   "' instead?",
                   SV_ARG(segment), SV_ARG(def->name));
        return false;
      }

      *out_res = (PathRes){
          .kind = PATHRES_VARIANT,
          .type = enum_ty,
          .as.variant.enum_def = def,
          .as.variant.def = variant,
      };
      return true;
    }
    default:
      break;
    }
  }

  assert(false && "unhandled PathResKind in cctx_resolve_path");
  return false;
}

bool rctx_resolve_path(ResolveCtx *ctx, Path *path, PathRes *out_res) {
  PathResCtx path_ctx = {
      .path = path,
      // .vscope = NULL,
      .tscope = ctx->tyres.tscope,
      .diags = ctx->diags,
      .tyres = &ctx->tyres,
      .al = ctx->al,
  };
  return resolve_path(&path_ctx, out_res);
}

bool cctx_resolve_path(CheckCtx *ctx, Path *path, PathRes *out_res) {
  PathResCtx path_ctx = {
      .path = path,
      // .vscope = ctx->vscope,
      .tscope = ctx->tscope,
      .diags = ctx->diags,
      .tyres = &ctx->tyres,
      .al = ctx->al,
  };
  return resolve_path(&path_ctx, out_res);
}