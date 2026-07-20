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
  case TY_ARRAY: {
    Type *elem = subst_apply(s, t->as.array.elem_type, al);
    return elem != t->as.array.elem_type ? ty_array(elem, al) : t;
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

// drop entries from `outer` whose name matches one of `shadow` (e.g. a
// function's own type params shadowing its enclosing impl's). subst_apply
// matches TY_GENERIC nodes by name only, not by which scope introduced them,
// so once an inner declaration reuses an outer name, every occurrence of
// that name in the inner scope can only mean the inner generic — keeping the
// outer binding around would make subst_apply silently substitute the wrong
// value wherever the names collide.
static Subst subst_exclude_shadowed(Subst outer, Type **shadow,
                                    int shadow_count, Allocator *al) {
  if (outer.count == 0 || shadow_count == 0) {
    return outer;
  }
  StringView *names = al_alloc(al, sizeof(StringView) * outer.count);
  Type **args = al_alloc(al, sizeof(Type *) * outer.count);
  int n = 0;
  for (int i = 0; i < outer.count; i++) {
    bool shadowed = false;
    for (int j = 0; j < shadow_count; j++) {
      assert(shadow[j]->kind == TY_GENERIC);
      if (sv_equal(outer.params[i], shadow[j]->as.generic.name)) {
        shadowed = true;
        break;
      }
    }
    if (!shadowed) {
      names[n] = outer.params[i];
      args[n] = outer.args[i];
      n++;
    }
  }
  Subst s;
  subst_init(&s, names, args, n);
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

  // `!` (never) coerces to any type: the code producing it diverges.
  if (a->kind == TY_NEVER || b->kind == TY_NEVER) {
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
  case TY_ARRAY:
    return infer_unify(ctx, a->as.array.elem_type, b->as.array.elem_type, diags,
                       span);
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

  case TY_ARRAY: {
    Type *elem = infer_apply(ctx, ty->as.array.elem_type, al);
    return elem != ty->as.array.elem_type ? ty_array(elem, al) : ty;
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
  tc->t_never = ty_never();
  tc->t_poison = ty_poison();

  tc->diags = diags;
  tc->al = al;

  impl_index_init(&tc->impl_index, al);
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

static void tc_register_impl(TypeChecker *tc, Module *m, Decl *decl) {
  assert(decl->kind == DECL_IMPL && "expected impl decl");

  DeclImpl *impl_decl = &decl->as.impl_decl;
  assert(m->impl_cap > m->impl_count && "impl capacity exceeded");

  ImplDef *def = al_alloc_zero_for(tc->al, ImplDef);
  // stub
  def->type_param_count = impl_decl->type_param_count;
  def->type_params =
      al_alloc_zero(tc->al, sizeof(StringView) * impl_decl->type_param_count);

  for (int i = 0; i < impl_decl->item_count; i++) {
    ImplItemNode *item = &impl_decl->items[i];
    if (item->kind == IMPL_ITEM_METHOD) {
      def->method_count++;
    } else if (item->kind == IMPL_ITEM_ASSOC_TYPE) {
      def->assoc_type_count++;
    } else {
      assert(false && "unhandled impl item kind");
    }
  }

  def->assoc_types =
      al_alloc_zero(tc->al, sizeof(AssocTypeDef) * def->assoc_type_count);
  def->methods = al_alloc_zero(tc->al, sizeof(MethodDef) * def->method_count);

  // register in module
  m->impls[m->impl_count++] = def;
  // set backpointers
  decl->as.impl_decl.def = def;
  def->module = m;
}

// builtins available in every module. for now just `print`:
//   fun print<T>(value: T) -> ()
// codegen lowers calls to it to OP_PRINT.
static void tc_register_builtins(TypeChecker *tc, Module *m) {
  FunDef *def = al_alloc_zero_for(tc->al, FunDef);
  def->name = sv_from_cstr("print");
  def->module = m;

  def->type_param_count = 1;
  def->type_params = al_alloc_zero(tc->al, sizeof(Type *));
  def->type_params[0] = ty_generic(sv_from_cstr("T"), NULL, 0, tc->al);

  def->param_count = 1;
  def->params = al_alloc_zero(tc->al, sizeof(ParamDef));
  def->params[0].name = sv_from_cstr("value");
  def->params[0].param_type = def->type_params[0];

  def->return_type = tc->t_unit;
  def->fun_type = ty_fun(&def->type_params[0], 1, tc->t_unit, tc->al);

  VarEntry *ve = NULL;
  vscope_define(&m->vscope, def->name, def->fun_type, tc->diags, (Span){0},
                &ve);
  ve->as.fun = def;

  TypeEntry *te = NULL;
  tscope_define(&m->tscope, def->name, def->fun_type, tc->diags, (Span){0},
                &te);
  te->as.fun_def = def;
}

static void tc_register_trait(TypeChecker *tc, Module *m, Decl *decl) {
  assert(decl->kind == DECL_TRAIT && "expected trait decl");

  DeclTrait *trait_decl = &decl->as.trait_decl;

  // minimal registration: the trait can be named (e.g. as an impl's trait
  // head); trait items are not resolved or checked yet.
  TraitDef *def = al_alloc_zero_for(tc->al, TraitDef);
  def->is_pub = decl->is_pub;
  def->name = trait_decl->name;

  trait_decl->def = def;
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
    case DECL_IMPL:
      m->impl_cap++;
      break;
    case DECL_TRAIT:
    case DECL_USE:
      break;
    default:
      assert(false && "unhandled decl kind in tc_register_module");
    }
  }

  // allocate
  m->funs = al_alloc_zero(tc->al, sizeof(FunDef *) * m->fun_cap);
  m->structs = al_alloc_zero(tc->al, sizeof(StructDef *) * m->struct_cap);
  m->enums = al_alloc_zero(tc->al, sizeof(EnumDef *) * m->enum_cap);
  m->impls = al_alloc_zero(tc->al, sizeof(ImplDef *) * m->impl_cap);

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
    case DECL_IMPL:
      tc_register_impl(tc, m, decl);
      break;
    case DECL_TRAIT:
      tc_register_trait(tc, m, decl);
      break;
    case DECL_USE:
      // todo: import linking (see compiler_phase_register)
      break;
    default:
      assert(false && "unhandled decl kind in tc_register_module");
    }
  }

  assert(m->fun_count == m->fun_cap && "fun count mismatch after registration");

  tc_register_builtins(tc, m);
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

static void resolve_impl_decl(ResolveCtx *rctx, Decl *decl) {
  assert(decl->kind == DECL_IMPL && "expected impl decl");
  DeclImpl *impl_decl = &decl->as.impl_decl;
  ImplDef *impl_def = impl_decl->def;

  for (int i = 0; i < impl_decl->type_param_count; i++) {
    if (impl_decl->type_params[i].inline_bound.refs != NULL) {
      diag_error(rctx->diags, impl_decl->type_params[i].span,
                 "inline bounds on impl type parameters are not supported");
    }
    impl_def->type_params[i] =
        ty_generic(impl_decl->type_params[i].name, NULL, 0, rctx->al);
  }

  // begin; impl level type scope
  rctx->tyres.tscope = tscope_push(rctx->tyres.tscope, rctx->al);

  for (int i = 0; i < impl_def->type_param_count; i++) {
    tscope_define(rctx->tyres.tscope, impl_decl->type_params[i].name,
                  impl_def->type_params[i], rctx->diags,
                  impl_decl->type_params[i].span, NULL);
  }

  // resolve self
  Type *self_ty = rctx_resolve(rctx, impl_decl->self_type);
  impl_def->self_type = self_ty;

  TypeEntry *self_te = NULL;
  tscope_define(rctx->tyres.tscope, sv_from_cstr("Self"), self_ty, rctx->diags,
                decl->span, &self_te);
  if (self_ty->kind == TY_STRUCT) {
    self_te->as.struct_def = self_ty->as.struc.def;
  } else if (self_ty->kind == TY_ENUM) {
    self_te->as.enum_def = self_ty->as.enm.def;
  }

  // resolve trait
  if (impl_decl->trait_type) {
    Type *trait_ty = rctx_resolve(rctx, impl_decl->trait_type);
    impl_def->trait_type = trait_ty;

    if (!type_is_poison(trait_ty) && trait_ty->kind != TY_TRAIT) {
      char buf[64];
      type_sprintf(trait_ty, buf, sizeof(buf));
      diag_error(rctx->diags, impl_decl->trait_type->span,
                 "impl trait type must be a trait, but got '%s'", buf);
    }
  }

  // register in the impl index before resolving items so `Self.Assoc` inside
  // this impl's own methods can be looked up
  impl_index_add(&rctx->tc->impl_index, impl_def);

  // resolve associated types before methods so a method signature can
  // reference `Self.Assoc`
  for (int i = 0, assoc_idx = 0; i < impl_decl->item_count; i++) {
    ImplItemNode *item = &impl_decl->items[i];
    if (item->kind != IMPL_ITEM_ASSOC_TYPE) {
      continue;
    }
    AssocTypeDef *assoc_def = &impl_def->assoc_types[assoc_idx++];
    assoc_def->name = item->name;
    assoc_def->type = rctx_resolve(rctx, item->assoc_type);
  }

  // resolve items
  for (int i = 0, method_idx = 0; i < impl_decl->item_count; i++) {
    ImplItemNode *item = &impl_decl->items[i];

    if (item->kind == IMPL_ITEM_METHOD) {
      MethodDef *method_def = &impl_def->methods[method_idx++];

      DeclFun *fun_decl = &item->fun_decl->as.fun_decl;
      FunDef *fun_def = al_alloc_zero_for(rctx->al, FunDef);
      method_def->fun = fun_def;
      method_def->name = fun_decl->name;

      fun_def->name = fun_decl->name;
      fun_def->module = impl_def->module;

      // resolve method type parameters
      fun_def->type_param_count = fun_decl->type_param_count;
      fun_def->type_params = al_alloc_zero(
          rctx->al, sizeof(StringView) * fun_decl->type_param_count);
      for (int j = 0; j < fun_def->type_param_count; j++) {
        if (fun_decl->type_params[j].inline_bound.refs != NULL) {
          diag_error(
              rctx->diags, fun_decl->type_params[j].span,
              "inline bounds on method type parameters are not supported");
        }
        fun_def->type_params[j] =
            ty_generic(fun_decl->type_params[j].name, NULL, 0, rctx->al);
      }

      // begin; method level type scope
      rctx->tyres.tscope = tscope_push(rctx->tyres.tscope, rctx->al);

      for (int i = 0; i < fun_def->type_param_count; i++) {
        tscope_define(rctx->tyres.tscope, fun_decl->type_params[i].name,
                      fun_def->type_params[i], rctx->diags,
                      fun_decl->type_params[i].span, NULL);
      }

      // resolve parameters and return type
      TypeScratch param_types;
      ts_init(&param_types, fun_decl->param_count, rctx->al);

      fun_def->param_count = fun_decl->param_count;
      fun_def->params =
          al_alloc_zero(rctx->al, sizeof(ParamDef) * fun_decl->param_count);

      for (int j = 0; j < fun_decl->param_count; j++) {
        param_types.ptr[j] =
            fun_decl->params[j].is_self
                ? self_ty
                : rctx_resolve(rctx, fun_decl->params[j].type_annotation);

        fun_def->params[j].name = fun_decl->params[j].name;
        fun_def->params[j].is_self = fun_decl->params[j].is_self;
        fun_def->params[j].param_type = param_types.ptr[j];
      }

      fun_def->return_type = fun_decl->return_type
                                 ? rctx_resolve(rctx, fun_decl->return_type)
                                 : rctx->tc->t_unit;

      // end; method level type scope
      rctx->tyres.tscope = tscope_pop(rctx->tyres.tscope);

      fun_def->fun_type = ty_fun(param_types.ptr, param_types.count,
                                 fun_def->return_type, rctx->al);
    } else if (item->kind == IMPL_ITEM_ASSOC_TYPE) {
      // resolved in the pre-pass above
    } else {
      assert(false && "unhandled impl item kind");
    }
  }

  // end; impl level type scope
  rctx->tyres.tscope = tscope_pop(rctx->tyres.tscope);
}

static void resolve_trait_decl(ResolveCtx *rctx, Decl *decl) {
  assert(decl->kind == DECL_TRAIT && "expected trait decl");
  DeclTrait *trait_decl = &decl->as.trait_decl;
  TraitDef *trait_def = trait_decl->def;

  Type *trait_ty = ty_trait(trait_def, rctx->al);
  trait_def->self_type = trait_ty;

  TypeEntry *te = NULL;
  tscope_define(rctx->tyres.tscope, trait_def->name, trait_ty, rctx->diags,
                decl->span, &te);
  te->as.trait_def = trait_def;
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
  case DECL_IMPL:
    resolve_impl_decl(rctx, decl);
    break;
  case DECL_TRAIT:
    resolve_trait_decl(rctx, decl);
    break;
  case DECL_USE:
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
      BindingPatTuple *pat = &var->binding.as.tuple;

      if (type_is_poison(init_ty)) {
        for (int i = 0; i < pat->count; i++) {
          vscope_define(ctx->vscope, pat->names[i], ctx->tc->t_poison,
                        ctx->diags, stmt->span, NULL);
        }
        break;
      }

      switch (init_ty->kind) {
      case TY_TUPLE: {
        if (init_ty->as.tuple.elem_count != pat->count) {
          diag_error(
              ctx->diags, stmt->span,
              "tuple destructuring count mismatch: expected %d but got %d",
              pat->count, init_ty->as.tuple.elem_count);
          break;
        }

        for (int i = 0; i < pat->count; i++) {
          Type *ty = init_ty->as.tuple.elem_types[i];
          vscope_define(ctx->vscope, pat->names[i], ty, ctx->diags, stmt->span,
                        NULL);
        }
        break;
      }
      case TY_STRUCT: {
        StructDef *def = init_ty->as.struc.def;
        if (!def->is_tuple) {
          char ty_buf[64];
          type_sprintf(init_ty, ty_buf, sizeof(ty_buf));
          diag_error(ctx->diags, stmt->span,
                     "cannot destructure type '%s' as a tuple", ty_buf);
          break;
        }
        if (def->field_count != pat->count) {
          diag_error(
              ctx->diags, stmt->span,
              "tuple destructuring count mismatch: expected %d but got %d",
              pat->count, def->field_count);
          break;
        }

        for (int i = 0; i < pat->count; i++) {
          Type *ty = def->fields[i].type;
          vscope_define(ctx->vscope, pat->names[i], ty, ctx->diags, stmt->span,
                        NULL);
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
    case BIND_STRUCT: {
      BindingPatStruct *pat = &var->binding.as.struc;

      if (type_is_poison(init_ty)) {
        for (int i = 0; i < pat->field_count; i++) {
          vscope_define(ctx->vscope, pat->field_names[i], ctx->tc->t_poison,
                        ctx->diags, stmt->span, NULL);
        }
        break;
      }

      if (init_ty->kind != TY_STRUCT) {
        char ty_buf[64];
        type_sprintf(init_ty, ty_buf, sizeof(ty_buf));
        diag_error(ctx->diags, stmt->span,
                   "cannot destructure type '%s' as a struct", ty_buf);
        break;
      }

      PathRes r;
      if (!cctx_resolve_path(ctx, &pat->path, &r)) {
        break;
      }

      if (r.kind != PATHRES_TYPE || r.type->kind != TY_STRUCT) {
        char ty_buf[64];
        type_sprintf(r.type, ty_buf, sizeof(ty_buf));
        diag_error(ctx->diags, stmt->span,
                   "cannot destructure type '%s' as a struct", ty_buf);
        break;
      }

      if (r.type->as.struc.def != init_ty->as.struc.def) {
        char expected_buf[64], actual_buf[64];
        type_sprintf(r.type, expected_buf, sizeof(expected_buf));
        type_sprintf(init_ty, actual_buf, sizeof(actual_buf));
        diag_error(
            ctx->diags, stmt->span,
            "struct destructuring type mismatch: expected '%s' but got '%s'",
            expected_buf, actual_buf);
        break;
      }

      StructDef *def = init_ty->as.struc.def;
      if (def->is_tuple) {
        diag_error(ctx->diags, stmt->span,
                   "cannot destructure tuple struct '" SV_FMT
                   "' with a struct pattern; use tuple destructuring: "
                   "var (a, b) = ...",
                   SV_ARG(def->name));
        for (int i = 0; i < pat->field_count; i++) {
          vscope_define(ctx->vscope, pat->field_names[i], ctx->tc->t_poison,
                        ctx->diags, stmt->span, NULL);
        }
        break;
      }

      for (int i = 0; i < pat->field_count; i++) {
        StringView name = pat->field_names[i];
        FieldIdent ident = {.name = name};
        FieldDef *field_def =
            find_struct_field(def, ident, ctx->diags, stmt->span);
        if (!field_def) {
          continue;
        }
        vscope_define(ctx->vscope, name, field_def->type, ctx->diags,
                      stmt->span, NULL);
      }
      break;
    }
    default:
      assert(false && "unhandled var binding kind in resolve_stmt");
    }
    break;
  }
  case STMT_BREAK: {
    if (!ctx->loop_depth) {
      diag_error(ctx->diags, stmt->span, "break statement not within a loop");
    }
    break;
  }
  case STMT_CONTINUE: {
    if (!ctx->loop_depth) {
      diag_error(ctx->diags, stmt->span,
                 "continue statement not within a loop");
    }
    break;
  }
  case STMT_RETURN: {
    StmtReturn *ret = &stmt->as.return_stmt;
    Type *val_ty = ret->value != NULL
                       ? resolve_expr(ctx, ret->value, ctx->return_type)
                       : ctx->tc->t_unit;

    if (ctx->return_type == NULL) {
      diag_error(ctx->diags, stmt->span,
                 "return statement not within a function");
      break;
    }

    if (!type_is_poison(val_ty) && !type_is_poison(ctx->return_type)) {
      infer_unify(&ctx->infer, ctx->return_type, val_ty, ctx->diags,
                  stmt->span);
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

  // a single-segment path naming a value binding (local var, param, closure)
  // is a call through a first-class function value. generic functions still
  // go through path resolution below so their type params are instantiated
  // into `subst`.
  Path *callee_path = &callee->as.path_expr.path;
  if (callee_path->count == 1 && callee_path->segments[0].type_arg_count == 0) {
    VarEntry *ve =
        vscope_lookup(ctx->vscope, callee_path->segments[0].name, NULL);
    if (ve != NULL && (ve->type->kind != TY_FUNCTION || ve->as.fun == NULL ||
                       ve->as.fun->type_param_count == 0)) {
      return resolve_expr(ctx, callee, NULL);
    }
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
      // the impl-level subst may map its generics to fresh unknowns (bare
      // generic self like `Point::new`); pass it up so arguments are unified
      // rather than compared strictly. it was already applied into r.type.
      *subst = r.as.method.subst;
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
    // still bind pattern names (as poison) so later uses don't cascade
    for (int i = 0; i < pattern->as.struc.field_count; i++) {
      FieldPat *fp = &pattern->as.struc.fields[i];
      if (fp->sub_pattern != NULL) {
        check_pattern(ctx, fp->sub_pattern, ctx->tc->t_poison);
      } else {
        vscope_define(ctx->vscope, fp->ident.name, ctx->tc->t_poison,
                      ctx->diags, fp->span, NULL);
      }
    }
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
    // still bind pattern names (as poison) so later uses don't cascade
    for (int i = 0; i < pattern->as.variant.field_count; i++) {
      FieldPat *fp = &pattern->as.variant.fields[i];
      if (fp->sub_pattern != NULL) {
        check_pattern(ctx, fp->sub_pattern, ctx->tc->t_poison);
      } else {
        vscope_define(ctx->vscope, fp->ident.name, ctx->tc->t_poison,
                      ctx->diags, fp->span, NULL);
      }
    }
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

static Type *resolve_method_call_expr(CheckCtx *ctx, Expr *expr, Type *hint) {
  (void)hint;
  ExprMethodCall *mc = &expr->as.method_call;

  Type *self_ty = resolve_expr(ctx, mc->object, NULL);
  if (type_is_poison(self_ty)) {
    return ctx->tc->t_poison;
  }

  ImplMatch match;
  MethodDef *method =
      impl_index_method(&ctx->tc->impl_index, self_ty, mc->method_name, &match,
                        &ctx->infer, expr->span, ctx->al);
  if (!method) {
    char self_buf[64];
    type_sprintf(self_ty, self_buf, sizeof(self_buf));
    diag_error(ctx->diags, expr->span,
               "no method named '" SV_FMT "' found for type '%s'",
               SV_ARG(mc->method_name), self_buf);
    return ctx->tc->t_poison;
  }

  mc->resolved_method = method;
  mc->resolved_impl = match.impl;

  FunDef *fun = method->fun;

  int self_idx = -1;
  for (int i = 0; i < fun->param_count; i++) {
    if (fun->params[i].is_self) {
      self_idx = i;
      break;
    }
  }
  if (self_idx < 0) {
    diag_error(ctx->diags, expr->span,
               "'" SV_FMT "' is an associated function, not a method — it has "
               "no 'self' parameter",
               SV_ARG(mc->method_name));
    return ctx->tc->t_poison;
  }

  if (mc->type_arg_count > 0 && mc->type_arg_count != fun->type_param_count) {
    diag_error(ctx->diags, expr->span, "expected %d type arguments but got %d",
               fun->type_param_count, mc->type_arg_count);
    return ctx->tc->t_poison;
  }

  TypeScratch explicit_type_args;
  ts_init(&explicit_type_args, mc->type_arg_count, ctx->al);
  for (int i = 0; i < mc->type_arg_count; i++) {
    explicit_type_args.ptr[i] = tyres_resolve(&ctx->tyres, mc->type_args[i]);
    if (type_is_poison(explicit_type_args.ptr[i])) {
      return ctx->tc->t_poison;
    }
  }

  // combine the impl-level substitution (already concrete, from matching
  // self_ty) with the method's own generics (fresh unknowns / explicit type
  // args) into a single subst, so subst_apply sees every generic that can
  // appear in fun->fun_type in one pass. see subst_exclude_shadowed for why
  // impl-level entries shadowed by the method's own type params are dropped.
  Subst method_subst =
      infer_open_generics(&ctx->infer, fun->type_params, explicit_type_args.ptr,
                          fun->type_param_count, expr->span, ctx->al);
  bool is_generic = method_subst.count > 0;

  Subst impl_subst = subst_exclude_shadowed(match.subst, fun->type_params,
                                            fun->type_param_count, ctx->al);

  int n_impl = impl_subst.count;
  int n_total = n_impl + method_subst.count;

  Subst subst = subst_empty();
  if (n_total > 0) {
    StringView *names = al_alloc(ctx->al, sizeof(StringView) * n_total);
    Type **args = al_alloc(ctx->al, sizeof(Type *) * n_total);
    if (n_impl > 0) {
      memcpy(names, impl_subst.params, sizeof(StringView) * n_impl);
      memcpy(args, impl_subst.args, sizeof(Type *) * n_impl);
    }
    if (method_subst.count > 0) {
      memcpy(names + n_impl, method_subst.params,
             sizeof(StringView) * method_subst.count);
      memcpy(args + n_impl, method_subst.args,
             sizeof(Type *) * method_subst.count);
    }
    subst_init(&subst, names, args, n_total);
  }

  Type *fun_ty = subst_apply(&subst, fun->fun_type, ctx->al);

  int expected_argc = fun->param_count - 1;
  if (mc->arg_count != expected_argc) {
    diag_error(ctx->diags, expr->span, "expected %d arguments but got %d",
               expected_argc, mc->arg_count);
    return ctx->tc->t_poison;
  }

  bool had_error = false;
  for (int i = 0, k = 0; i < fun->param_count; i++) {
    if (i == self_idx) {
      continue;
    }
    Type *param_ty = fun_ty->as.fun.param_types[i];
    Expr *arg = mc->args[k++];
    Type *arg_ty = resolve_expr(ctx, arg, param_ty);

    if (type_is_poison(arg_ty) || type_is_poison(param_ty)) {
      had_error = true;
      continue;
    }

    if (is_generic) {
      had_error |=
          !infer_unify(&ctx->infer, arg_ty, param_ty, ctx->diags, arg->span);
    } else if (!types_equal(arg_ty, param_ty)) {
      char ab[64], pb[64];
      type_sprintf(arg_ty, ab, sizeof(ab));
      type_sprintf(param_ty, pb, sizeof(pb));
      diag_error(ctx->diags, arg->span,
                 "argument %d: expected '%s' but got '%s'", k, pb, ab);
      had_error = true;
    }
  }

  if (had_error) {
    return ctx->tc->t_poison;
  }

  Type *ret_ty = fun_ty->as.fun.return_type;
  if (is_generic) {
    ret_ty = infer_apply(&ctx->infer, ret_ty, ctx->al);
  }

  return ret_ty;
}

// a "Result-like" enum has exactly two single-field tuple variants named
// `Ok` and `Err`. `?` is defined structurally against this shape until a
// proper Try trait exists.
static bool enum_is_resultish(EnumDef *def, VariantDef **out_ok,
                              VariantDef **out_err) {
  if (def->variant_count != 2) {
    return false;
  }

  VariantDef *ok = NULL, *err = NULL;
  for (int i = 0; i < def->variant_count; i++) {
    VariantDef *v = &def->variants[i];
    if (sv_equal_cstr(v->name, "Ok")) {
      ok = v;
    } else if (sv_equal_cstr(v->name, "Err")) {
      err = v;
    }
  }

  if (!ok || !err || !ok->is_tuple || ok->field_count != 1 || !err->is_tuple ||
      err->field_count != 1) {
    return false;
  }

  *out_ok = ok;
  *out_err = err;
  return true;
}

static Type *resolve_propagate_expr(CheckCtx *ctx, Expr *expr) {
  ExprPropagate *prop = &expr->as.propagate;

  Type *op_ty = resolve_expr(ctx, prop->operand, ctx->return_type);
  op_ty = infer_apply(&ctx->infer, op_ty, ctx->al);

  if (type_is_poison(op_ty)) {
    return ctx->tc->t_poison;
  }

  VariantDef *op_ok = NULL, *op_err = NULL;
  if (op_ty->kind != TY_ENUM ||
      !enum_is_resultish(op_ty->as.enm.def, &op_ok, &op_err)) {
    char buf[64];
    type_sprintf(op_ty, buf, sizeof(buf));
    diag_error(ctx->diags, prop->operand->span,
               "the '?' operator requires a Result-like enum (variants "
               "'Ok(T)' and 'Err(E)'), got '%s'",
               buf);
    return ctx->tc->t_poison;
  }

  EnumDef *def = op_ty->as.enm.def;

  Type *ret_ty = ctx->return_type != NULL
                     ? infer_apply(&ctx->infer, ctx->return_type, ctx->al)
                     : ctx->tc->t_unit;
  if (type_is_poison(ret_ty)) {
    return ctx->tc->t_poison;
  }

  if (ret_ty->kind != TY_ENUM || ret_ty->as.enm.def != def) {
    char ob[64], rb[64];
    type_sprintf(op_ty, ob, sizeof(ob));
    type_sprintf(ret_ty, rb, sizeof(rb));
    diag_error(ctx->diags, expr->span,
               "'?' propagates '%s', so the enclosing function must return "
               "the same enum, but it returns '%s'",
               ob, rb);
    return ctx->tc->t_poison;
  }

  Type *ok_payload = op_ok->fields[0].type;
  Type *op_err_payload = op_err->fields[0].type;
  Type *ret_err_payload = op_err->fields[0].type;

  if (def->type_param_count > 0) {
    Subst op_subst = infer_open_generics(
        &ctx->infer, def->type_params, op_ty->as.enm.type_args,
        def->type_param_count, expr->span, ctx->al);
    ok_payload = infer_apply(
        &ctx->infer, subst_apply(&op_subst, ok_payload, ctx->al), ctx->al);
    op_err_payload = infer_apply(
        &ctx->infer, subst_apply(&op_subst, op_err_payload, ctx->al), ctx->al);

    Subst ret_subst = infer_open_generics(
        &ctx->infer, def->type_params, ret_ty->as.enm.type_args,
        def->type_param_count, expr->span, ctx->al);
    ret_err_payload =
        infer_apply(&ctx->infer,
                    subst_apply(&ret_subst, ret_err_payload, ctx->al), ctx->al);
  }

  if (!infer_unify(&ctx->infer, ret_err_payload, op_err_payload, ctx->diags,
                   expr->span)) {
    return ctx->tc->t_poison;
  }

  return ok_payload;
}

static Type *resolve_closure_expr(CheckCtx *ctx, Expr *expr, Type *hint) {
  ExprClosure *closure = &expr->as.closure;

  // a function-typed hint with matching arity lets unannotated params and the
  // return type flow in from the expected type
  Type *fun_hint = NULL;
  if (hint != NULL) {
    Type *h = infer_find(&ctx->infer, hint);
    if (h->kind == TY_FUNCTION &&
        h->as.fun.param_count == closure->param_count) {
      fun_hint = h;
    }
  }

  FunDef *def = al_alloc_zero_for(ctx->al, FunDef);
  def->is_closure = true;
  def->module = ctx->fun != NULL ? ctx->fun->module : NULL;
  def->param_count = closure->param_count;
  def->params = al_alloc_zero(ctx->al, sizeof(ParamDef) * closure->param_count);
  closure->def = def;

  TypeScratch param_types;
  ts_init(&param_types, closure->param_count, ctx->al);

  for (int i = 0; i < closure->param_count; i++) {
    ClosureParam *cp = &closure->params[i];

    Type *pty = NULL;
    if (cp->type_annotation != NULL) {
      pty = tyres_resolve(&ctx->tyres, cp->type_annotation);
    } else if (fun_hint != NULL) {
      pty = infer_find(&ctx->infer, fun_hint->as.fun.param_types[i]);
    } else {
      pty = infer_fresh(&ctx->infer, cp->name, NULL, cp->span);
    }

    param_types.ptr[i] = pty;
    def->params[i].name = cp->name;
    def->params[i].is_self = cp->is_self;
    def->params[i].param_type = pty;
  }

  Type *ret_ty = NULL;
  if (closure->return_type_annotation != NULL) {
    ret_ty = tyres_resolve(&ctx->tyres, closure->return_type_annotation);
  } else if (fun_hint != NULL) {
    ret_ty = infer_find(&ctx->infer, fun_hint->as.fun.return_type);
  } else {
    ret_ty = infer_fresh(&ctx->infer, (StringView){0}, NULL, expr->span);
  }
  def->return_type = ret_ty;

  // check the body in the closure's own function context
  FunDef *saved_fun = ctx->fun;
  Type *saved_ret = ctx->return_type;
  int saved_loop_depth = ctx->loop_depth;
  ctx->fun = def;
  ctx->return_type = ret_ty;
  ctx->loop_depth = 0; // break/continue can't escape the closure body

  ctx->vscope = vscope_push(ctx->vscope, true, false, ctx->al);
  for (int i = 0; i < closure->param_count; i++) {
    vscope_define(ctx->vscope, closure->params[i].name, param_types.ptr[i],
                  ctx->diags, closure->params[i].span, NULL);
  }

  Type *body_ty = resolve_expr(ctx, closure->body, ret_ty);
  if (!type_is_poison(body_ty) && !type_is_poison(ret_ty)) {
    infer_unify(&ctx->infer, ret_ty, body_ty, ctx->diags, closure->body->span);
  }

  ctx->vscope = vscope_pop(ctx->vscope);
  ctx->fun = saved_fun;
  ctx->return_type = saved_ret;
  ctx->loop_depth = saved_loop_depth;

  Type *fun_ty = ty_fun(param_types.ptr, closure->param_count, ret_ty, ctx->al);
  def->fun_type = fun_ty;
  return fun_ty;
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
      result = resolve_expr(ctx, block->tail_expr, hint);
    } else if (block->stmt_count > 0 &&
               block->stmts[block->stmt_count - 1]->kind == STMT_RETURN) {
      // a block ending in `return` never falls through
      result = ctx->tc->t_never;
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

    if (op == TOKEN_PLUS && lhs->kind == TY_STRING && rhs->kind == TY_STRING) {
      result = ty_string();
    } else if (is_arith) {
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

    bool crossed_fn = false;
    VarEntry *ve = vscope_lookup(ctx->vscope, name, &crossed_fn);
    if (!ve) {
      diag_error(ctx->diags, expr->span, "undefined variable '%.*s'", name.len,
                 name.chars);
      result = ctx->tc->t_poison;
      break;
    }

    if (crossed_fn) {
      ve->is_captured = true; // referenced from inside a nested closure
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
  case EXPR_SELF: {
    VarEntry *ve = vscope_lookup(ctx->vscope, sv_from_cstr("self"), NULL);
    if (!ve) {
      diag_error(ctx->diags, expr->span, "'self' is not available here");
      result = ctx->tc->t_poison;
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
  case EXPR_WHILE: {
    ExprWhile *wh = &expr->as.while_expr;

    Type *cond_ty = resolve_expr(ctx, wh->condition, NULL);
    if (type_is_poison(cond_ty)) {
      result = ctx->tc->t_poison;
      break;
    }

    if (cond_ty->kind != TY_BOOL) {
      char ct[64];
      type_sprintf(cond_ty, ct, sizeof(ct));
      diag_error(ctx->diags, wh->condition->span,
                 "while loop condition must be Bool, got '%s'", ct);
      result = ctx->tc->t_poison;
      break;
    }

    ctx->vscope = vscope_push(ctx->vscope, false, true, ctx->al);
    ctx->loop_depth++;
    resolve_expr(ctx, wh->body, NULL);
    ctx->vscope = vscope_pop(ctx->vscope);
    ctx->loop_depth--;

    result = ctx->tc->t_unit;
    break;
  }
  case EXPR_FOR: {
    ExprFor *for_ = &expr->as.for_expr;

    Type *iter_ty = resolve_expr(ctx, for_->iterable, NULL);
    if (type_is_poison(iter_ty)) {
      result = ctx->tc->t_poison;
      break;
    }

    Type *item_ty = NULL;
    if (iter_ty->kind == TY_ARRAY) {
      item_ty = iter_ty->as.array.elem_type;
    } else if (iter_ty->kind == TY_RANGE) {
      item_ty = ctx->tc->t_int;
    } else {
      char it[64];
      type_sprintf(iter_ty, it, sizeof(it));
      diag_error(ctx->diags, for_->iterable->span,
                 "cannot iterate over type '%s' in for loop", it);
      result = ctx->tc->t_poison;
      break;
    }

    ctx->vscope = vscope_push(ctx->vscope, false, true, ctx->al);
    vscope_define(ctx->vscope, for_->var_name, item_ty, ctx->diags,
                  for_->var_span, NULL);
    ctx->loop_depth++;
    resolve_expr(ctx, for_->body, NULL);
    ctx->vscope = vscope_pop(ctx->vscope);
    ctx->loop_depth--;

    result = ctx->tc->t_unit;
    break;
  }
  case EXPR_IF: {
    ExprIf *if_ = &expr->as.if_expr;

    Type *cond_ty = resolve_expr(ctx, if_->condition, NULL);
    if (type_is_poison(cond_ty)) {
      result = ctx->tc->t_poison;
      break;
    }

    if (cond_ty->kind != TY_BOOL) {
      char ct[64];
      type_sprintf(cond_ty, ct, sizeof(ct));
      diag_error(ctx->diags, if_->condition->span,
                 "if condition must be Bool, got '%s'", ct);
      result = ctx->tc->t_poison;
      break;
    }

    Type *then_ty = resolve_expr(ctx, if_->then_block, NULL);
    Type *else_ty = NULL;
    if (if_->else_branch != NULL) {
      else_ty = resolve_expr(ctx, if_->else_branch, NULL);
    } else {
      else_ty = ctx->tc->t_unit;
    }

    if (!type_is_poison(then_ty) && !type_is_poison(else_ty)) {
      Span mismatch_span =
          if_->else_branch != NULL ? if_->else_branch->span : expr->span;
      if (!infer_unify(&ctx->infer, then_ty, else_ty, ctx->diags,
                       mismatch_span)) {
        result = ctx->tc->t_poison;
        break;
      }
    }

    // they are unified; prefer the branch that doesn't diverge
    result = then_ty->kind == TY_NEVER ? else_ty : then_ty;
    break;
  }
  case EXPR_METHOD_CALL:
    result = resolve_method_call_expr(ctx, expr, hint);
    break;
  case EXPR_UNIT:
    result = ctx->tc->t_unit;
    break;
  case EXPR_POISON:
    result = ctx->tc->t_poison;
    break;
  case EXPR_UNARY: {
    ExprUnary *unary = &expr->as.unary;
    bool is_not = unary->op == TOKEN_NOT;

    Type *op_ty =
        resolve_expr(ctx, unary->operand, is_not ? ctx->tc->t_bool : hint);
    op_ty = infer_find(&ctx->infer, op_ty);

    if (type_is_poison(op_ty)) {
      result = ctx->tc->t_poison;
      break;
    }

    if (is_not) {
      if (op_ty->kind != TY_BOOL) {
        char buf[64];
        type_sprintf(op_ty, buf, sizeof(buf));
        diag_error(ctx->diags, unary->operand->span,
                   "'not' requires a Bool operand, got '%s'", buf);
        result = ctx->tc->t_poison;
      } else {
        result = ctx->tc->t_bool;
      }
    } else {
      if (!type_is_numeric(op_ty)) {
        char buf[64];
        type_sprintf(op_ty, buf, sizeof(buf));
        diag_error(ctx->diags, unary->operand->span,
                   "unary '-' requires a numeric type, got '%s'", buf);
        result = ctx->tc->t_poison;
      } else {
        result = op_ty;
      }
    }
    break;
  }
  case EXPR_RANGE: {
    ExprRange *range = &expr->as.range;
    Type *start_ty = resolve_expr(ctx, range->start, ctx->tc->t_int);
    Type *end_ty = resolve_expr(ctx, range->end, ctx->tc->t_int);

    bool ok = !type_is_poison(start_ty) && !type_is_poison(end_ty);
    if (ok) {
      ok &= infer_unify(&ctx->infer, ctx->tc->t_int, start_ty, ctx->diags,
                        range->start->span);
      ok &= infer_unify(&ctx->infer, ctx->tc->t_int, end_ty, ctx->diags,
                        range->end->span);
    }

    result = ok ? ty_range() : ctx->tc->t_poison;
    break;
  }
  case EXPR_CAST: {
    ExprCast *cast = &expr->as.cast;
    Type *target = tyres_resolve(&ctx->tyres, cast->target_type);
    Type *op_ty = resolve_expr(ctx, cast->operand, NULL);
    op_ty = infer_find(&ctx->infer, op_ty);

    if (type_is_poison(op_ty) || type_is_poison(target)) {
      result = ctx->tc->t_poison;
      break;
    }

    bool numeric_cast = (op_ty->kind == TY_INT && target->kind == TY_FLOAT) ||
                        (op_ty->kind == TY_FLOAT && target->kind == TY_INT);
    // todo: trait coercion via `as` once trait objects exist
    if (numeric_cast || types_equal(op_ty, target)) {
      result = target;
    } else {
      char ob[64], tb[64];
      type_sprintf(op_ty, ob, sizeof(ob));
      type_sprintf(target, tb, sizeof(tb));
      diag_error(ctx->diags, expr->span, "invalid cast from '%s' to '%s'", ob,
                 tb);
      result = ctx->tc->t_poison;
    }
    break;
  }
  case EXPR_CLOSURE:
    result = resolve_closure_expr(ctx, expr, hint);
    break;
  case EXPR_PROPAGATE:
    result = resolve_propagate_expr(ctx, expr);
    break;
  case EXPR_ARRAY: {
    ExprArray *array = &expr->as.array;

    Type *hint_ty = hint ? infer_find(&ctx->infer, hint) : NULL;
    Type *elem_hint = hint_ty && hint_ty->kind == TY_ARRAY
                          ? hint_ty->as.array.elem_type
                          : NULL;

    if (array->count == 0) {
      Type *elem_ty =
          elem_hint != NULL
              ? elem_hint
              : infer_fresh(&ctx->infer, (StringView){0}, NULL, expr->span);
      result = ty_array(elem_ty, ctx->al);
      break;
    }

    Type *elem_ty = resolve_expr(ctx, array->elems[0], elem_hint);
    bool ok = !type_is_poison(elem_ty);

    for (int i = 1; i < array->count; i++) {
      Type *ty = resolve_expr(ctx, array->elems[i], elem_ty);
      if (type_is_poison(ty)) {
        ok = false;
        continue;
      }
      if (ok) {
        ok &= infer_unify(&ctx->infer, elem_ty, ty, ctx->diags,
                          array->elems[i]->span);
      }
    }

    result = ok ? ty_array(infer_apply(&ctx->infer, elem_ty, ctx->al), ctx->al)
                : ctx->tc->t_poison;
    break;
  }
  case EXPR_INDEX: {
    ExprIndex *index = &expr->as.index;

    Type *obj_ty = resolve_expr(ctx, index->object, NULL);
    obj_ty = infer_apply(&ctx->infer, obj_ty, ctx->al);

    Type *idx_ty = resolve_expr(ctx, index->index, ctx->tc->t_int);

    if (type_is_poison(obj_ty) || type_is_poison(idx_ty)) {
      result = ctx->tc->t_poison;
      break;
    }

    if (obj_ty->kind != TY_ARRAY) {
      char buf[64];
      type_sprintf(obj_ty, buf, sizeof(buf));
      diag_error(ctx->diags, index->object->span,
                 "cannot index a value of type '%s'", buf);
      result = ctx->tc->t_poison;
      break;
    }

    if (!infer_unify(&ctx->infer, ctx->tc->t_int, idx_ty, ctx->diags,
                     index->index->span)) {
      result = ctx->tc->t_poison;
      break;
    }

    result = obj_ty->as.array.elem_type;
    break;
  }
  case EXPR_INTERPOLATED: {
    ExprInterpolated *interp = &expr->as.interpolated;
    for (int i = 0; i < interp->seg_count; i++) {
      InterpolSeg *seg = &interp->segs[i];
      if (seg->kind != ISEG_EXPR) {
        continue;
      }

      Type *seg_ty = resolve_expr(ctx, seg->expr, NULL);
      seg_ty = infer_find(&ctx->infer, seg_ty);
      if (type_is_poison(seg_ty)) {
        continue; // error already reported
      }

      switch (seg_ty->kind) {
      case TY_INT:
      case TY_FLOAT:
      case TY_BOOL:
      case TY_STRING:
        break;
      default: {
        char buf[64];
        type_sprintf(seg_ty, buf, sizeof(buf));
        diag_error(ctx->diags, seg->expr->span,
                   "cannot interpolate a value of type '%s' (user-defined "
                   "formatting is not yet supported)",
                   buf);
      }
      }
    }
    result = ctx->tc->t_string;
    break;
  }
  default:
    assert(false && "unhandled expr kind in resolve_expr");
  }

  assert(result && "result is null");
  expr->resolved_type = result;
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

static void tc_check_impl(TypeChecker *tc, Decl *decl) {
  assert(decl->kind == DECL_IMPL && "expected impl decl");
  DeclImpl *impl_decl = &decl->as.impl_decl;
  ImplDef *impl_def = impl_decl->def;

  CheckCtx cctx;
  cctx_init(&cctx, tc, impl_def->module, tc->diags, tc->al);

  // begin type scope
  cctx.tyres.tscope = tscope_push(cctx.tyres.tscope, cctx.al);

  for (int i = 0; i < impl_def->type_param_count; i++) {
    tscope_define(cctx.tyres.tscope, impl_decl->type_params[i].name,
                  impl_def->type_params[i], cctx.diags,
                  impl_decl->type_params[i].span, NULL);
  }

  TypeEntry *self_te = NULL;
  tscope_define(cctx.tyres.tscope, sv_from_cstr("Self"), impl_def->self_type,
                cctx.diags, decl->span, &self_te);
  if (impl_def->self_type->kind == TY_STRUCT) {
    self_te->as.struct_def = impl_def->self_type->as.struc.def;
  } else if (impl_def->self_type->kind == TY_ENUM) {
    self_te->as.enum_def = impl_def->self_type->as.enm.def;
  }

  for (int i = 0, method_idx = 0, assoc_idx = 0; i < impl_decl->item_count;
       i++) {
    ImplItemNode *item = &impl_decl->items[i];

    if (item->kind == IMPL_ITEM_METHOD) {
      MethodDef *method_def = &impl_def->methods[method_idx++];
      FunDef *fun_def = method_def->fun;
      DeclFun *fun_decl = &item->fun_decl->as.fun_decl;

      cctx.fun = fun_def;
      cctx.return_type = fun_def->return_type;

      // begin method type scope
      cctx.tyres.tscope = tscope_push(cctx.tyres.tscope, cctx.al);

      for (int j = 0; j < fun_def->type_param_count; j++) {
        tscope_define(cctx.tyres.tscope, fun_decl->type_params[j].name,
                      fun_def->type_params[j], cctx.diags,
                      fun_decl->type_params[j].span, NULL);
      }

      // begin method var scope
      cctx.vscope = vscope_push(cctx.vscope, true, false, cctx.al);
      for (int j = 0; j < fun_def->param_count; j++) {
        ParamDef *param_def = &fun_def->params[j];
        StringView param_name =
            param_def->is_self ? sv_from_cstr("self") : param_def->name;

        vscope_define(cctx.vscope, param_name, param_def->param_type,
                      cctx.diags, (Span){0}, NULL);
      }

      resolve_expr_coerced(&cctx, fun_decl->body, fun_def->return_type);

      // end method var scope
      cctx.vscope = vscope_pop(cctx.vscope);

      // end method type scope
      cctx.tyres.tscope = tscope_pop(cctx.tyres.tscope);
    } else if (item->kind == IMPL_ITEM_ASSOC_TYPE) {
      AssocTypeDef *assoc_def = &impl_def->assoc_types[assoc_idx++];
      (void)assoc_def;
    } else {
      assert(false && "unhandled impl item kind in tc_check_impl");
    }
  }

  // end type scope
  cctx.tyres.tscope = tscope_pop(cctx.tyres.tscope);
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
    case DECL_TRAIT: // trait items (default methods) are not checked yet
    case DECL_USE:
      break;
    case DECL_IMPL:
      tc_check_impl(tc, decl);
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

static Type *impl_index_assoc_type(ImplIndex *idx, Type *self_type,
                                   StringView name, Allocator *al);

Type *tyres_resolve(TypeResolver *r, TypeNode *node) {
  if (node->resolved) {
    return node->resolved;
  }

  Type *result = NULL;

  switch (node->kind) {
  case TYNODE_UNIT:
    result = r->tc->t_unit;
    break;
  case TYNODE_SELF: {
    TypeEntry *e = tscope_lookup(r->tscope, sv_from_cstr("Self"));
    if (!e) {
      diag_error(r->diags, node->span, "'Self' is not valid outside an impl");
      result = r->tc->t_poison;
      break;
    }
    result = e->type;
    break;
  }
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
  case TYNODE_ARRAY: {
    Type *elem = tyres_resolve(r, node->as.array.elem);
    result = type_is_poison(elem) ? r->tc->t_poison : ty_array(elem, r->al);
    break;
  }
  case TYNODE_ASSOC: {
    TypeNodeAssoc *assoc = &node->as.assoc;
    Type *base = tyres_resolve(r, assoc->base);

    if (type_is_poison(base)) {
      result = base;
      break;
    }

    if (base->kind == TY_GENERIC) {
      diag_error(
          r->diags, node->span,
          "associated types on generic type parameters are not yet supported");
      result = r->tc->t_poison;
      break;
    }

    result = impl_index_assoc_type(&r->tc->impl_index, base, assoc->assoc_name,
                                   r->al);
    if (result == NULL) {
      char buf[64];
      type_sprintf(base, buf, sizeof(buf));
      diag_error(r->diags, node->span,
                 "type '%s' has no associated type '" SV_FMT "'", buf,
                 SV_ARG(assoc->assoc_name));
      result = r->tc->t_poison;
    }
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
// ImplIndex
// ═══════════════════════════════════════════════════════════════════════════════

void impl_index_init(ImplIndex *idx, Allocator *al) {
  idx->all = NULL;
  idx->count = 0;
  idx->cap = 0;
  idx->al = al;
}

void impl_index_add(ImplIndex *idx, ImplDef *impl) {
  if (idx->count >= idx->cap) {
    int new_cap = idx->cap == 0 ? 4 : idx->cap * 2;
    idx->all = al_realloc(idx->al, idx->all, sizeof(ImplDef *) * idx->cap,
                          sizeof(ImplDef *) * new_cap);
    assert(idx->all && "out of memory");
    idx->cap = new_cap;
  }
  idx->all[idx->count++] = impl;
}

// structurally match `pattern` (an impl's self_type, possibly containing
// TY_GENERIC leaves bound to `param_names`) against a concrete `concrete`
// type, recording bindings into `out_args`/`bound`. returns false on a
// structural mismatch or a binding conflict.
static bool impl_type_match(Type *pattern, Type *concrete,
                            StringView *param_names, int param_count,
                            Type **out_args, bool *bound) {
  if (pattern->kind == TY_GENERIC) {
    for (int i = 0; i < param_count; i++) {
      if (!sv_equal(param_names[i], pattern->as.generic.name)) {
        continue;
      }
      if (bound[i]) {
        return types_equal(out_args[i], concrete);
      }
      bound[i] = true;
      out_args[i] = concrete;
      return true;
    }
    return false; // generic not owned by this impl
  }

  if (pattern->kind != concrete->kind) {
    return false;
  }

  switch (pattern->kind) {
  case TY_STRUCT: {
    TypeStruct *ps = &pattern->as.struc, *cs = &concrete->as.struc;
    if (ps->def != cs->def || ps->type_arg_count != cs->type_arg_count) {
      return false;
    }
    for (int i = 0; i < ps->type_arg_count; i++) {
      if (!impl_type_match(ps->type_args[i], cs->type_args[i], param_names,
                           param_count, out_args, bound)) {
        return false;
      }
    }
    return true;
  }
  case TY_ENUM: {
    TypeEnum *pe = &pattern->as.enm, *ce = &concrete->as.enm;
    if (pe->def != ce->def || pe->type_arg_count != ce->type_arg_count) {
      return false;
    }
    for (int i = 0; i < pe->type_arg_count; i++) {
      if (!impl_type_match(pe->type_args[i], ce->type_args[i], param_names,
                           param_count, out_args, bound)) {
        return false;
      }
    }
    return true;
  }
  case TY_ARRAY:
    return impl_type_match(pattern->as.array.elem_type,
                           concrete->as.array.elem_type, param_names,
                           param_count, out_args, bound);
  case TY_TUPLE: {
    TypeTuple *pt = &pattern->as.tuple, *ct = &concrete->as.tuple;
    if (pt->elem_count != ct->elem_count) {
      return false;
    }
    for (int i = 0; i < pt->elem_count; i++) {
      if (!impl_type_match(pt->elem_types[i], ct->elem_types[i], param_names,
                           param_count, out_args, bound)) {
        return false;
      }
    }
    return true;
  }
  default:
    return types_equal(pattern, concrete);
  }
}

// find the associated type `self_type.name` among registered impls. returns
// the resolved type with the matched impl's type params substituted, or NULL
// if no impl defines it.
static Type *impl_index_assoc_type(ImplIndex *idx, Type *self_type,
                                   StringView name, Allocator *al) {
  if (type_is_poison(self_type)) {
    return NULL;
  }

  for (int i = 0; i < idx->count; i++) {
    ImplDef *impl = idx->all[i];
    if (impl->assoc_type_count == 0 || impl->self_type == NULL ||
        type_is_poison(impl->self_type)) {
      continue;
    }

    Subst subst = subst_empty();
    bool matched;

    if (impl->type_param_count == 0) {
      matched = types_equal(impl->self_type, self_type);
    } else {
      int n = impl->type_param_count;
      StringView *names = al_alloc(al, sizeof(StringView) * n);
      Type **args = al_alloc(al, sizeof(Type *) * n);
      bool *bound = al_alloc_zero(al, sizeof(bool) * n);
      for (int j = 0; j < n; j++) {
        names[j] = impl->type_params[j]->as.generic.name;
      }

      matched =
          impl_type_match(impl->self_type, self_type, names, n, args, bound);
      for (int j = 0; matched && j < n; j++) {
        matched = bound[j];
      }
      if (matched) {
        subst_init(&subst, names, args, n);
      }
    }

    if (!matched) {
      continue;
    }

    for (int j = 0; j < impl->assoc_type_count; j++) {
      if (sv_equal(impl->assoc_types[j].name, name)) {
        Type *t = impl->assoc_types[j].type;
        if (t == NULL) {
          return NULL; // not resolved yet (self-referential pre-pass)
        }
        return subst.count > 0 ? subst_apply(&subst, t, al) : t;
      }
    }
  }

  return NULL;
}

// true when `t` is a generic type's canonical self — the uninstantiated
// `Point<T>` a bare path like `Point::new` resolves to. no concrete impl
// self type can ever equal it, so method lookup must select by name instead.
static bool type_is_bare_generic_self(Type *t) {
  if (t->kind == TY_STRUCT) {
    return t->as.struc.def->type_param_count > 0 &&
           t == t->as.struc.def->self_type;
  }
  if (t->kind == TY_ENUM) {
    return t->as.enm.def->type_param_count > 0 && t == t->as.enm.def->self_type;
  }
  return false;
}

static bool type_same_nominal_def(Type *a, Type *b) {
  if (a->kind != b->kind) {
    return false;
  }
  if (a->kind == TY_STRUCT) {
    return a->as.struc.def == b->as.struc.def;
  }
  if (a->kind == TY_ENUM) {
    return a->as.enm.def == b->as.enm.def;
  }
  return false;
}

MethodDef *impl_index_method(ImplIndex *idx, Type *self_type, StringView name,
                             ImplMatch *out_match, InferCtx *infer, Span span,
                             Allocator *al) {
  if (type_is_poison(self_type)) {
    return NULL;
  }

  // bare generic self: select the impl by method name and open its type
  // params as fresh unknowns — argument unification at the call site pins
  // down the type arguments (e.g. `Point::new(10, 20)` selecting
  // `impl Point<Int>`). first name match wins.
  if (infer != NULL && type_is_bare_generic_self(self_type)) {
    for (int i = 0; i < idx->count; i++) {
      ImplDef *impl = idx->all[i];
      if (impl->self_type == NULL || type_is_poison(impl->self_type) ||
          !type_same_nominal_def(impl->self_type, self_type)) {
        continue;
      }

      for (int j = 0; j < impl->method_count; j++) {
        if (!sv_equal(impl->methods[j].name, name)) {
          continue;
        }

        Subst subst = subst_empty();
        if (impl->type_param_count > 0) {
          subst = infer_open_generics(infer, impl->type_params, NULL,
                                      impl->type_param_count, span, al);
        }
        if (out_match) {
          out_match->impl = impl;
          out_match->subst = subst;
        }
        return &impl->methods[j];
      }
    }
    return NULL;
  }

  for (int i = 0; i < idx->count; i++) {
    ImplDef *impl = idx->all[i];
    if (type_is_poison(impl->self_type)) {
      continue;
    }

    Subst subst = subst_empty();
    bool matched;

    if (impl->type_param_count == 0) {
      matched = types_equal(impl->self_type, self_type);
    } else {
      int n = impl->type_param_count;
      StringView *names = al_alloc(al, sizeof(StringView) * n);
      Type **args = al_alloc(al, sizeof(Type *) * n);
      bool *bound = al_alloc_zero(al, sizeof(bool) * n);
      for (int j = 0; j < n; j++) {
        names[j] = impl->type_params[j]->as.generic.name;
      }

      matched =
          impl_type_match(impl->self_type, self_type, names, n, args, bound);
      for (int j = 0; matched && j < n; j++) {
        matched = bound[j]; // every impl type param must appear in self_type
      }
      if (matched) {
        subst_init(&subst, names, args, n);
      }
    }

    if (!matched) {
      continue;
    }

    for (int j = 0; j < impl->method_count; j++) {
      if (sv_equal(impl->methods[j].name, name)) {
        if (out_match) {
          out_match->impl = impl;
          out_match->subst = subst;
        }
        return &impl->methods[j];
      }
    }
  }

  return NULL;
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
    case PATHRES_CTX_STRUCT: {
      Type *struct_ty = res_ctx.scope.struct_.inst;

      if (!is_last) {
        diag_error(ctx->diags, path->span,
                   "cannot access member '" SV_FMT "' of associated item",
                   SV_ARG(segment));
        return false;
      }

      if (path->segments[i].type_arg_count > 0) {
        diag_error(ctx->diags, path->span,
                   "cannot apply type arguments to associated function '" SV_FMT
                   "'",
                   SV_ARG(segment));
        return false;
      }

      ImplMatch match;
      MethodDef *method =
          impl_index_method(&ctx->tyres->tc->impl_index, struct_ty, segment,
                            &match, ctx->tyres->infer, path->span, ctx->al);
      if (!method) {
        char ty_buf[64];
        type_sprintf(struct_ty, ty_buf, sizeof(ty_buf));
        diag_error(ctx->diags, path->span,
                   "no associated item named '" SV_FMT "' found for type '%s'",
                   SV_ARG(segment), ty_buf);
        return false;
      }

      FunDef *fun = method->fun;
      Subst subst = subst_exclude_shadowed(match.subst, fun->type_params,
                                           fun->type_param_count, ctx->al);
      Type *fun_ty = subst_apply(&subst, fun->fun_type, ctx->al);

      *out_res = (PathRes){
          .kind = PATHRES_METHOD,
          .type = fun_ty,
          .as.method.fun = fun,
          .as.method.subst = subst,
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
      .tscope = ctx->tyres.tscope,
      .diags = ctx->diags,
      .tyres = &ctx->tyres,
      .al = ctx->al,
  };
  return resolve_path(&path_ctx, out_res);
}