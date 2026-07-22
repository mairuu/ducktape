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
  case TY_TUPLE: {
    TypeScratch elems;
    ts_init(&elems, t->as.tuple.elem_count, al);
    bool changed = false;
    for (int i = 0; i < t->as.tuple.elem_count; i++) {
      elems.ptr[i] = subst_apply(s, t->as.tuple.elem_types[i], al);
      changed |= elems.ptr[i] != t->as.tuple.elem_types[i];
    }
    return changed ? ty_tuple(elems.ptr, elems.count, al) : t;
  }
  case TY_ARRAY: {
    Type *elem = subst_apply(s, t->as.array.elem_type, al);
    return elem != t->as.array.elem_type ? ty_array(elem, al) : t;
  }
  case TY_ASSOC: {
    // `T.Item` follows T. The projection can't be collapsed here (the binding
    // lives on an impl, and subst_apply has no index); infer_apply does that
    // once the base is concrete.
    Type *base = subst_apply(s, t->as.assoc.base, al);
    return base != t->as.assoc.base
               ? ty_assoc(base, t->as.assoc.assoc_name, t->as.assoc.trait, al)
               : t;
  }
  default:
    return t;
  }
}

static Type *impl_index_assoc_type(ImplIndex *idx, Type *self_type,
                                   StringView name, Allocator *al);
static bool impl_applies(ImplDef *impl, Type *self_type, Subst *out_subst,
                         ImplIndex *bounds_idx, Allocator *al);
static TraitDef *impl_trait_def(ImplDef *impl);

static void infer_note_explicit_bound(InferCtx *ctx, Type *param, Type *arg,
                                      Span span) {
  if (ctx->explicit_bound_count == ctx->explicit_bound_cap) {
    int new_cap =
        ctx->explicit_bound_cap == 0 ? 4 : ctx->explicit_bound_cap * 2;
    ctx->explicit_bounds =
        al_realloc(ctx->al, ctx->explicit_bounds,
                   sizeof(ExplicitBound) * ctx->explicit_bound_cap,
                   sizeof(ExplicitBound) * new_cap);
    ctx->explicit_bound_cap = new_cap;
  }
  ctx->explicit_bounds[ctx->explicit_bound_count++] =
      (ExplicitBound){.param = param, .arg = arg, .span = span};
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
    // a bounded param carries its source TY_GENERIC as the unknown's `bound`,
    // so infer_check_bounds can enforce the trait bounds once it's solved.
    Type *bound = params[i]->as.generic.bound_count > 0 ? params[i] : NULL;
    if (concretes && concretes[i]) {
      args[i] = concretes[i];
      // an explicit type argument bypasses inference entirely; queue its
      // bounds so infer_check_bounds still gets to see them.
      if (bound != NULL) {
        infer_note_explicit_bound(ctx, params[i], concretes[i], span);
      }
    } else {
      args[i] = infer_fresh(ctx, params[i]->as.generic.name, bound, span);
    }
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

// append `inner`'s entries after `outer`'s, so one subst_apply pass sees every
// generic in scope at a call site (an impl's type params and the method's own).
// Entries of `outer` shadowed by a name in `inner` must already have been
// dropped — see subst_exclude_shadowed for why, and because subst_apply takes
// the first match, which would otherwise be the outer one.
static Subst subst_concat(Subst outer, Subst inner, Allocator *al) {
  if (outer.count == 0) {
    return inner;
  }
  if (inner.count == 0) {
    return outer;
  }
  int n = outer.count + inner.count;
  StringView *names = al_alloc(al, sizeof(StringView) * n);
  Type **args = al_alloc(al, sizeof(Type *) * n);
  memcpy(names, outer.params, sizeof(StringView) * outer.count);
  memcpy(args, outer.args, sizeof(Type *) * outer.count);
  memcpy(names + outer.count, inner.params, sizeof(StringView) * inner.count);
  memcpy(args + outer.count, inner.args, sizeof(Type *) * inner.count);
  Subst s;
  subst_init(&s, names, args, n);
  return s;
}

// bind `Self` to the receiver's type, on top of a method's own type args. A
// trait's default body is generic over `Self` (see resolve_trait_default_impl),
// so this is the substitution that instantiates it. Appended last: a method
// type parameter could not be named `Self`, but subst_apply takes the first
// match and the method's own args must win at every other name.
static Subst subst_with_self(Subst method_args, Type *self_ty, Allocator *al) {
  int n = method_args.count + 1;
  StringView *names = al_alloc(al, sizeof(StringView) * (size_t)n);
  Type **args = al_alloc(al, sizeof(Type *) * (size_t)n);
  for (int i = 0; i < method_args.count; i++) {
    names[i] = method_args.params[i];
    args[i] = method_args.args[i];
  }
  names[method_args.count] = sv_from_cstr("Self");
  args[method_args.count] = self_ty;

  Subst s;
  subst_init(&s, names, args, n);
  return s;
}

// ── instantiation records ────────────────────────────────────────────────────

// Stash on a call node the substitution that instantiated its target, so
// codegen can monomorphise it. `s` is copied rather than aliased: the caller's
// arrays are its own working substitution, and the copy is what
// cctx_solve_insts later rewrites in place — the arguments recorded here are
// still the call's fresh unknowns.
static void cctx_record_inst(CheckCtx *ctx, Subst *slot, Subst s) {
  *slot = subst_empty();
  if (s.count == 0) {
    return;
  }
  StringView *params = al_alloc(ctx->al, sizeof(StringView) * s.count);
  Type **args = al_alloc(ctx->al, sizeof(Type *) * s.count);
  memcpy(params, s.params, sizeof(StringView) * s.count);
  memcpy(args, s.args, sizeof(Type *) * s.count);
  subst_init(slot, params, args, s.count);

  if (ctx->pending_inst_count == ctx->pending_inst_cap) {
    int new_cap = ctx->pending_inst_cap == 0 ? 8 : ctx->pending_inst_cap * 2;
    ctx->pending_insts = al_realloc(ctx->al, ctx->pending_insts,
                                    sizeof(Subst *) * ctx->pending_inst_cap,
                                    sizeof(Subst *) * new_cap);
    ctx->pending_inst_cap = new_cap;
  }
  ctx->pending_insts[ctx->pending_inst_count++] = slot;
}

// Rewrite every recorded instantiation against the solved inference state.
// Run after infer_finalize, whose diagnostics cover the arguments that stayed
// free; those keep a TY_UNKNOWN here, which is exactly what makes codegen
// refuse to instantiate rather than emit a slot for the wrong copy.
static void cctx_solve_insts(CheckCtx *ctx) {
  for (int i = 0; i < ctx->pending_inst_count; i++) {
    Subst *s = ctx->pending_insts[i];
    for (int j = 0; j < s->count; j++) {
      s->args[j] = infer_apply(&ctx->infer, s->args[j], ctx->al);
    }
  }
  ctx->pending_inst_count = 0;
}

// ═══════════════════════════════════════════════════════════════════════════════
// Inference
// ═══════════════════════════════════════════════════════════════════════════════

void infer_init(InferCtx *ctx, ImplIndex *impls, Allocator *al) {
  memset(ctx, 0, sizeof(*ctx));
  ctx->impls = impls;
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

  // a projection whose base has been solved is really whatever the impl bound
  // it to — collapse it before comparing, or `T.Item` and `Int` would look
  // like different kinds.
  if (a->kind == TY_ASSOC) {
    a = infer_apply(ctx, a, ctx->al);
  }
  if (b->kind == TY_ASSOC) {
    b = infer_apply(ctx, b, ctx->al);
  }

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
  case TY_TUPLE: {
    TypeTuple *at = &a->as.tuple, *bt = &b->as.tuple;
    if (at->elem_count != bt->elem_count) {
      char ab[64], bb[64];
      type_sprintf(a, ab, sizeof(ab));
      type_sprintf(b, bb, sizeof(bb));
      diag_error(diags, span, "type mismatch: expected '%s' but got '%s'", ab,
                 bb);
      return false;
    }
    bool ok = true;
    for (int i = 0; i < at->elem_count; i++)
      ok &= infer_unify(ctx, at->elem_types[i], bt->elem_types[i], diags, span);
    return ok;
  }

  case TY_ARRAY:
    return infer_unify(ctx, a->as.array.elem_type, b->as.array.elem_type, diags,
                       span);

  case TY_TRAIT:
  case TY_DYN:
  case TY_ASSOC: {
    // both are interned, so anything that reaches here (they weren't
    // types_equal) is a genuine mismatch — `T.Item` vs `U.Item`, or two
    // different traits. There is nothing to decompose. Note that
    // `dyn Show` vs a concrete `Sq` never reaches this switch at all: the
    // kinds differ, so it is caught above — and a *coercion* is offered
    // there instead (see check_coerce).
    char ab[64], bb[64];
    type_sprintf(a, ab, sizeof(ab));
    type_sprintf(b, bb, sizeof(bb));
    diag_error(diags, span, "type mismatch: expected '%s' but got '%s'", ab,
               bb);
    return false;
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

  case TY_ASSOC: {
    // `T.Item` is a placeholder: once T is solved, the projection collapses to
    // whatever the applicable impl bound that associated type to. While the
    // base is still abstract (a free unknown, or a type parameter inside a
    // generic function's own body) it stays a projection.
    Type *base = infer_apply(ctx, ty->as.assoc.base, al);
    Type *bound = NULL;
    if (base->kind != TY_UNKNOWN && base->kind != TY_GENERIC &&
        base->kind != TY_TRAIT && ctx->impls != NULL) {
      bound =
          impl_index_assoc_type(ctx->impls, base, ty->as.assoc.assoc_name, al);
    }
    if (bound != NULL) {
      return infer_apply(ctx, bound, al);
    }
    return base != ty->as.assoc.base
               ? ty_assoc(base, ty->as.assoc.assoc_name, ty->as.assoc.trait, al)
               : ty;
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

  case TY_TUPLE: {
    TypeTuple *t = &ty->as.tuple;
    TypeScratch elems;
    ts_init(&elems, t->elem_count, al);
    bool changed = false;

    for (int i = 0; i < t->elem_count; i++) {
      elems.ptr[i] = infer_apply(ctx, t->elem_types[i], al);
      changed |= elems.ptr[i] != t->elem_types[i];
    }
    return changed ? ty_tuple(elems.ptr, elems.count, al) : ty;
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

// An impl that would have applied had `m` imported it. Since impls became
// module-granular this is the likeliest reason a lookup fails, and it is the
// one thing the visible set cannot tell you — so a "diagnostics only" list of
// every impl in the program is consulted here and nowhere else. `pred` asks
// whether a candidate is the one being missed.
static ImplDef *find_unimported_impl(TypeChecker *tc, ImplIndex *visible,
                                     bool (*pred)(ImplDef *, void *),
                                     void *ctx) {
  for (int i = 0; i < tc->all_impls.count; i++) {
    ImplDef *impl = tc->all_impls.all[i];
    bool seen = false;
    for (int j = 0; j < visible->count && !seen; j++) {
      seen = visible->all[j] == impl;
    }
    if (!seen && pred(impl, ctx)) {
      return impl;
    }
  }
  return NULL;
}

static void note_unimported_impl(DiagBag *diags, ImplDef *impl) {
  if (impl == NULL) {
    return;
  }
  diag_note(diags, (Span){0},
            "an applicable impl exists in module '" SV_FMT
            "', but this module does not import it",
            SV_ARG(impl->module->file_path));
}

// An impl that matched `self_type` structurally but was rejected because one
// of its *own* type params failed a bound — `impl<T: Display> Display for [T]`
// meeting `[Widget]`. Without this note the diagnostic says only that
// `[Widget]` is not Display, which is true and useless: the impl exists, and
// the thing to fix is one level down. Same role as `note_unimported_impl` — the
// visible set cannot tell you *why* it declined. `trait` narrows the search, or
// NULL to consider every impl — which is what the "no method named" caller
// wants, since a method may come from any of them.
static void note_blocking_bound(DiagBag *diags, ImplIndex *idx, Type *self_type,
                                TraitDef *trait, StringView method,
                                Allocator *al) {
  for (int i = 0; i < idx->count; i++) {
    ImplDef *impl = idx->all[i];
    if (impl->type_param_count == 0 ||
        (trait != NULL && impl_trait_def(impl) != trait)) {
      continue;
    }
    if (method.len > 0) {
      bool declares = false;
      for (int j = 0; j < impl->method_count && !declares; j++) {
        declares = sv_equal(impl->methods[j].name, method);
      }
      if (!declares) {
        continue;
      }
    }
    Subst subst;
    if (!impl_applies(impl, self_type, &subst, NULL, al)) {
      continue; // didn't match even structurally
    }
    for (int j = 0; j < impl->type_param_count; j++) {
      Type *param = impl->type_params[j];
      if (param == NULL || param->kind != TY_GENERIC) {
        continue;
      }
      Type *arg = subst_find(&subst, param->as.generic.name);
      if (arg == NULL) {
        continue;
      }
      for (int b = 0; b < param->as.generic.bound_count; b++) {
        TraitDef *need = param->as.generic.bounds[b];
        if (impl_index_implements(idx, arg, need, al)) {
          continue;
        }
        char arg_buf[64];
        type_sprintf(arg, arg_buf, sizeof(arg_buf));
        diag_note(diags, (Span){0},
                  "an impl exists, but it requires '%s: " SV_FMT "'", arg_buf,
                  SV_ARG(need->name));
        return;
      }
    }
  }
}

typedef struct {
  Type *self_type;
  TraitDef *trait;   // NULL to match any
  StringView method; // empty to match any
  ImplIndex *impls;  // visible set, to answer the candidate's own bounds
  Allocator *al;
} ImplQuery;

static bool impl_query_matches(ImplDef *impl, void *raw) {
  ImplQuery *q = raw;
  if (q->trait != NULL && impl_trait_def(impl) != q->trait) {
    return false;
  }
  // bounds are asked against the *visible* set, the same one selection would
  // have used — otherwise the note offers an import that would not have helped.
  Subst ignored;
  if (!impl_applies(impl, q->self_type, &ignored, q->impls, q->al)) {
    return false;
  }
  if (q->method.len == 0) {
    return true;
  }
  for (int i = 0; i < impl->method_count; i++) {
    if (sv_equal(impl->methods[i].name, q->method)) {
      return true;
    }
  }
  return false;
}

// After inference is solved, enforce that every bounded type parameter was
// instantiated with a type implementing each of its trait bounds. A bounded
// unknown carries its source TY_GENERIC in `.bound` (see infer_open_generics);
// unsolved unknowns are left to infer_finalize.
// report every bound of `param` (a TY_GENERIC) that `arg` fails to implement.
static void check_bounds_satisfied(TypeChecker *tc, ImplIndex *idx, Type *param,
                                   Type *arg, DiagBag *diags, Span span,
                                   Allocator *al) {
  for (int b = 0; b < param->as.generic.bound_count; b++) {
    TraitDef *trait = param->as.generic.bounds[b];
    if (!impl_index_implements(idx, arg, trait, al)) {
      char buf[64];
      type_sprintf(arg, buf, sizeof(buf));
      diag_error(diags, span, "type '%s' does not implement trait '" SV_FMT "'",
                 buf, SV_ARG(trait->name));
      ImplQuery q = {.self_type = arg, .trait = trait, .impls = idx, .al = al};
      note_unimported_impl(
          diags, find_unimported_impl(tc, idx, impl_query_matches, &q));
      note_blocking_bound(diags, idx, arg, trait, (StringView){0}, al);
    }
  }
}

void infer_check_bounds(InferCtx *ctx, TypeChecker *tc, DiagBag *diags,
                        Allocator *al) {
  ImplIndex *idx = ctx->impls; // the enclosing module's visible set
  for (uint32_t id = 0; id < ctx->next_id; id++) {
    Type *node = ctx->nodes[id];
    Type *g = node->as.unknown.bound;
    if (g == NULL || g->kind != TY_GENERIC || g->as.generic.bound_count == 0) {
      continue;
    }
    Type *sol = ctx->solutions[id] ? infer_find(ctx, ctx->solutions[id]) : NULL;
    if (sol == NULL || sol->kind == TY_UNKNOWN) {
      continue; // free variable — infer_finalize reports it
    }
    sol = infer_apply(ctx, sol, al);
    if (type_is_poison(sol)) {
      continue;
    }
    check_bounds_satisfied(tc, idx, g, sol, diags, node->as.unknown.intro_span,
                           al);
  }

  // and the type arguments written out explicitly, which never became unknowns
  for (int i = 0; i < ctx->explicit_bound_count; i++) {
    ExplicitBound *eb = &ctx->explicit_bounds[i];
    Type *arg = infer_apply(ctx, eb->arg, al);
    if (type_is_poison(arg) || arg->kind == TY_UNKNOWN) {
      continue;
    }
    check_bounds_satisfied(tc, idx, eb->param, arg, diags, eb->span, al);
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

  impl_index_init(&tc->all_impls, al);
}

void tc_destroy(TypeChecker *tc) { (void)tc; }

// bind `@native("io_print")` / `@intrinsic("array_len")` to what C registered
// under that name. This is the whole of the "resolved at link time" half of
// the design: the *signature* went through the ordinary checker, so all that
// is left is a name lookup — and doing it here rather than in codegen is what
// gives an unknown name a real span to report against, which a hand-built
// builtin never had.
static void tc_bind_native(TypeChecker *tc, FunDef *def, const AttrNode *attr) {
  def->native_kind = attr->kind;
  def->native_name = attr->name;

  switch (attr->kind) {
  case ATTR_NONE:
    return;
  case ATTR_NATIVE:
    def->native = native_lookup(attr->name);
    if (def->native == NULL) {
      diag_error(tc->diags, attr->span, "no native named '" SV_FMT "'",
                 SV_ARG(attr->name));
      diag_note(tc->diags, (Span){0}, "available: %s", native_names());
      def->native_kind = ATTR_NONE;
    }
    return;
  case ATTR_INTRINSIC: {
    int op = intrinsic_lookup(attr->name);
    if (op < 0) {
      diag_error(tc->diags, attr->span, "no intrinsic named '" SV_FMT "'",
                 SV_ARG(attr->name));
      diag_note(tc->diags, (Span){0}, "available: %s", intrinsic_names());
      def->native_kind = ATTR_NONE;
      return;
    }
    def->intrinsic_op = (uint8_t)op;
    return;
  }
  }
}

static void tc_register_fun(TypeChecker *tc, Module *m, Decl *decl) {
  assert(decl->kind == DECL_FUN && "expected fun decl");

  DeclFun *fun_decl = &decl->as.fun_decl;
  assert(m->fun_cap > m->fun_count && "fun capacity exceeded");

  FunDef *def = al_alloc_zero_for(tc->al, FunDef);
  tc_bind_native(tc, def, &fun_decl->attr);
  // stub
  def->is_pub = decl->is_pub;
  def->name = fun_decl->name;
  def->slot = FUN_SLOT_NONE;
  def->body = fun_decl->body;
  def->span = decl->span;
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

  // the one place the compiler learns a name from the standard library. It is
  // keyed on the module too, not just the spelling: a user trait called
  // `Display` is an ordinary trait, and interpolation will not route through
  // it. See TypeChecker.display_trait.
  if (mod_is_std(m, "fmt") && sv_equal_cstr(def->name, "Display")) {
    tc->display_trait = def;
  }
}

// the name a top-level decl introduces into the module's scopes, if any.
// impls are anonymous; `use` names are checked by tc_link_imports instead.
static bool decl_item_name(Decl *decl, StringView *out) {
  switch (decl->kind) {
  case DECL_FUN:
    *out = decl->as.fun_decl.name;
    return true;
  case DECL_STRUCT:
    *out = decl->as.struct_decl.name;
    return true;
  case DECL_ENUM:
    *out = decl->as.enum_decl.name;
    return true;
  case DECL_TRAIT:
    *out = decl->as.trait_decl.name;
    return true;
  default:
    return false;
  }
}

// neither vscope_define nor tscope_define detects collisions, and both lookups
// return the *first* match — so a redeclaration would silently lose to the
// original with no diagnostic. Catch it here, before anything is registered.
static void tc_check_duplicate_decls(TypeChecker *tc, Module *m) {
  for (int i = 0; i < m->ast->decl_count; i++) {
    StringView name;
    if (!decl_item_name(m->ast->decls[i], &name)) {
      continue;
    }

    for (int j = 0; j < i; j++) {
      StringView prev;
      if (decl_item_name(m->ast->decls[j], &prev) && sv_equal(prev, name)) {
        diag_error(tc->diags, m->ast->decls[i]->span,
                   "the name '%.*s' is defined multiple times in this module",
                   SV_ARG(name));
        break;
      }
    }
  }
}

void tc_register_module(TypeChecker *tc, Module *m) {
  tc_check_duplicate_decls(tc, m);

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
    case DECL_VAR:
    case DECL_POISON:
      break;
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
      // imports are linked in tc_link_imports, which must run after the
      // dependency has been *resolved* — see its comment in sema.h.
      break;
    case DECL_VAR:
      // globals have no runtime representation: every slot space the linker
      // builds is a *definition* table, and the VM's globals are functions.
      diag_error(tc->diags, decl->span,
                 "top-level 'var' is not supported; move it into a function");
      break;
    case DECL_POISON:
      break; // the parser already reported it
    }
  }

  assert(m->fun_count == m->fun_cap && "fun count mismatch after registration");
}

// ═══════════════════════════════════════════════════════════════════════════════
// Import linking
// ═══════════════════════════════════════════════════════════════════════════════

// find `name` among m's *own* top-level declarations.
//
// deliberately an AST scan rather than a scope lookup. m's scopes also hold
// m's own imports, so looking there would give *every* `use b::X` the meaning
// of `pub use` — re-export has to be something the source asked for, which is
// `mod_find_use_alias` below. Scanning the decls also puts `Decl.is_pub` in
// reach, which is what makes visibility checkable without adding a flag to
// every scope entry.
static Decl *mod_find_own_decl(Module *m, StringView name) {
  for (int i = 0; i < m->ast->decl_count; i++) {
    Decl *decl = m->ast->decls[i];
    StringView own;
    if (decl_item_name(decl, &own) && sv_equal(own, name)) {
      return decl;
    }
  }
  return NULL;
}

// copy one resolved entry from `src`'s scopes into `dst`'s under `alias`.
// a fun lives in both scopes, a struct/enum/trait only in the type scope.
// `src == dst` is the std case: giving a builtin a second name in place.
static void link_copy_entry(TypeChecker *tc, Module *dst, Module *src,
                            StringView name, StringView alias, Span span) {
  TypeEntry *src_te = tscope_lookup(&src->tscope, name);
  if (src_te != NULL) {
    TypeEntry *te = NULL;
    tscope_define(&dst->tscope, alias, src_te->type, tc->diags, span, &te);
    te->as = src_te->as;
  }

  VarEntry *src_ve = vscope_lookup(&src->vscope, name, NULL);
  if (src_ve != NULL) {
    VarEntry *ve = NULL;
    // VarEntry.slot is meaningless for a module-level binding (it numbers a
    // binding in the importer, not a callable): the runtime slot lives on the
    // FunDef, assigned program-wide by exe_link, and travels with `ve->as`.
    vscope_define(&dst->vscope, alias, src_ve->type, tc->diags, span, &ve);
    ve->as = src_ve->as;
  }
}

// return true if `alias` is already taken in m, reporting why.
static bool link_name_taken(TypeChecker *tc, Module *m, const UseAlias *a) {
  if (mod_find_own_decl(m, a->alias) != NULL) {
    diag_error(tc->diags, a->span,
               "'" SV_FMT "' is already declared in this module",
               SV_ARG(a->alias));
    return true;
  }
  // vscope_define/tscope_define don't detect duplicates and both lookups
  // return the first match, so an unchecked collision would silently let the
  // import win over whatever was defined later. Check here or not at all.
  if (tscope_lookup(&m->tscope, a->alias) != NULL ||
      vscope_lookup(&m->vscope, a->alias, NULL) != NULL) {
    diag_error(tc->diags, a->span, "'" SV_FMT "' is already imported",
               SV_ARG(a->alias));
    return true;
  }
  return false;
}

// find the `use` declaration in `m` that binds `name`. A `pub use` is the
// re-export: from outside, the alias is an item of `m` like any other, and the
// entry is already in m's scopes — linking copied it there when *m* was
// resolved, which topological order guarantees has happened.
static Decl *mod_find_use_alias(Module *m, StringView name) {
  for (int i = 0; i < m->ast->decl_count; i++) {
    Decl *decl = m->ast->decls[i];
    if (decl->kind != DECL_USE) {
      continue;
    }
    UseTarget *target = &decl->as.use_decl.target;
    for (int j = 0; j < target->count; j++) {
      if (sv_equal(target->aliases[j].alias, name)) {
        return decl;
      }
    }
  }
  return NULL;
}

// bring one `use`d item into `m` under its alias. There is one import kind:
// a std module is an ordinary registry entry, vetted by the same `pub` rule
// as any other dependency.
static void link_import_item(TypeChecker *tc, Module *m, Module *dep,
                             const UseAlias *a) {
  Decl *d = mod_find_own_decl(dep, a->name);
  bool via_reexport = false;
  if (d == NULL) {
    d = mod_find_use_alias(dep, a->name);
    via_reexport = d != NULL;
  }
  if (d == NULL) {
    diag_error(tc->diags, a->span,
               "module '" SV_FMT "' has no item named '" SV_FMT "'",
               SV_ARG(dep->file_path), SV_ARG(a->name));
    return;
  }
  if (!d->is_pub) {
    // an import that isn't re-exported is *not* an item of the module, so the
    // wording has to differ: nothing is being hidden, it was never offered.
    if (via_reexport) {
      diag_error(tc->diags, a->span,
                 "'" SV_FMT "' is imported by module '" SV_FMT
                 "' but not re-exported",
                 SV_ARG(a->name), SV_ARG(dep->file_path));
      diag_note(tc->diags, (Span){0}, "add 'pub' to its 'use' declaration");
      return;
    }
    diag_error(tc->diags, a->span,
               "'" SV_FMT "' is private in module '" SV_FMT "'",
               SV_ARG(a->name), SV_ARG(dep->file_path));
    diag_note(tc->diags, (Span){0}, "add 'pub' to its declaration");
    return;
  }

  if (link_name_taken(tc, m, a)) {
    return;
  }

  // dep is resolved (topological order), so its scopes carry real types.
  // if they don't, resolving dep poisoned the decl — stay quiet.
  link_copy_entry(tc, m, dep, a->name, a->alias, a->span);
}

void tc_link_imports(TypeChecker *tc, Module *m, ModuleRegistry *reg) {
  for (int i = 0; i < m->import_count; i++) {
    ModImport *imp = &m->imports[i];
    if (imp->module_index < 0) {
      continue; // discovery already diagnosed it; stay quiet
    }
    Module *dep = reg->modules[imp->module_index];

    UseTarget *target = &imp->decl->as.use_decl.target;
    for (int j = 0; j < target->count; j++) {
      link_import_item(tc, m, dep, &target->aliases[j]);
    }
  }
}

void tc_import_impls(TypeChecker *tc, Module *m, ModuleRegistry *reg) {
  for (int i = 0; i < m->import_count; i++) {
    ModImport *imp = &m->imports[i];
    if (imp->module_index < 0) {
      continue;
    }
    // the dependency's *visible* set, not its own list: reachability is
    // transitive, so importing a module brings whatever it could itself see.
    // That is what makes a `pub use`d type usable — its impls live in the
    // module the re-export came from, one step further away.
    ImplIndex *dep = &reg->modules[imp->module_index]->visible_impls;

    for (int j = 0; j < dep->count; j++) {
      ImplDef *impl = dep->all[j];
      for (int k = 0; k < m->visible_impls.count; k++) {
        ImplDef *other = m->visible_impls.all[k];
        if (other == impl || !impl_defs_conflict(impl, other, tc->al)) {
          continue;
        }
        // reported at the `use` that made the pair collide, because that is
        // the only span of the three modules involved that lies in *this*
        // module's source — and diagnostics are printed against it.
        char buf[64];
        type_sprintf(impl->self_type, buf, sizeof(buf));
        diag_error(tc->diags, imp->decl->as.use_decl.path.span,
                   "this import makes two implementations of trait '" SV_FMT
                   "' for type '%s' visible at once",
                   SV_ARG(impl_trait_def(impl)->name), buf);
        diag_note(tc->diags, (Span){0}, "  one is in module '" SV_FMT "'",
                  SV_ARG(impl->module->file_path));
        diag_note(tc->diags, (Span){0}, "  the other in module '" SV_FMT "'",
                  SV_ARG(other->module->file_path));
        break;
      }
      impl_index_add(&m->visible_impls, impl);
    }
  }
}

#define MAX_BOUNDS 16

// Resolve a trait bound (one or more `+`-joined trait refs) to TraitDefs,
// appending de-duplicated into `out`. A ref must name a trait in scope;
// anything else is diagnosed and skipped. A bound is never module-qualified —
// an imported trait is in scope under its alias, so the ref is a bare name.
static void resolve_bound_refs(ResolveCtx *rctx, const TraitBound *bound,
                               TraitDef **out, int *count) {
  for (int i = 0; i < bound->ref_count; i++) {
    const TraitRef *ref = &bound->refs[i];
    if (ref->path.count > 1) {
      diag_error(rctx->diags, ref->span,
                 "a trait bound is not qualified by module; import the trait "
                 "with 'use' and name it directly");
      continue;
    }
    StringView name = ref->path.segments[ref->path.count - 1].name;
    TypeEntry *te = tscope_lookup(rctx->tyres.tscope, name);
    if (te == NULL || te->type == NULL || te->type->kind != TY_TRAIT) {
      diag_error(rctx->diags, ref->span, "'" SV_FMT "' is not a trait",
                 SV_ARG(name));
      continue;
    }
    TraitDef *def = te->as.trait_def;
    bool dup = false;
    for (int j = 0; j < *count; j++) {
      if (out[j] == def) {
        dup = true;
        break;
      }
    }
    if (dup) {
      continue;
    }
    if (*count >= MAX_BOUNDS) {
      diag_error(rctx->diags, ref->span, "too many trait bounds (max %d)",
                 MAX_BOUNDS);
      return;
    }
    out[(*count)++] = def;
  }
}

// Build the TY_GENERIC for a declaration's type parameter, gathering bounds
// from both its inline `<T: A + B>` position and any `where T: C` predicate
// naming it. Associated-type predicates (`where T::Item: C`) are not supported.
static Type *resolve_generic_param(ResolveCtx *rctx, const TypeParamNode *param,
                                   const WhereClause *where) {
  TraitDef *bounds[MAX_BOUNDS];
  int count = 0;

  if (param->inline_bound.ref_count > 0) {
    resolve_bound_refs(rctx, &param->inline_bound, bounds, &count);
  }
  if (where != NULL) {
    for (int i = 0; i < where->pred_count; i++) {
      const WherePred *pred = &where->preds[i];
      if (pred->lhs.segment_count != 1) {
        diag_error(
            rctx->diags, pred->lhs.span,
            "associated-type bounds in `where` clauses are not supported");
        continue;
      }
      if (sv_equal(pred->lhs.segments[0], param->name)) {
        resolve_bound_refs(rctx, &pred->bound, bounds, &count);
      }
    }
  }
  return ty_generic(param->name, bounds, count, rctx->al);
}

static void resolve_fun_decl(ResolveCtx *rctx, Decl *decl) {
  assert(decl->kind == DECL_FUN && "expected fun decl");
  DeclFun *fun_decl = &decl->as.fun_decl;
  FunDef *fun_def = fun_decl->def;

  for (int i = 0; i < fun_decl->type_param_count; i++) {
    fun_def->type_params[i] = resolve_generic_param(
        rctx, &fun_decl->type_params[i], fun_decl->where_clause);
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
    struct_def->type_params[i] = resolve_generic_param(
        rctx, &struct_decl->type_params[i], struct_decl->where_clause);
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
    enum_def->type_params[i] = resolve_generic_param(
        rctx, &enum_decl->type_params[i], enum_decl->where_clause);
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
    impl_def->type_params[i] = resolve_generic_param(
        rctx, &impl_decl->type_params[i], impl_decl->where_clause);
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

  // register in the module's visible set before resolving items so
  // `Self.Assoc` inside this impl's own methods can be looked up. The set
  // already holds every imported impl (tc_import_impls ran first), so this is
  // also where a local impl meets an imported one.
  for (int i = 0; i < rctx->impls->count; i++) {
    ImplDef *other = rctx->impls->all[i];
    if (!impl_defs_conflict(impl_def, other, rctx->al)) {
      continue;
    }
    char buf[64];
    type_sprintf(impl_def->self_type, buf, sizeof(buf));
    diag_error(rctx->diags, decl->span,
               "conflicting implementations of trait '" SV_FMT
               "' for type '%s'",
               SV_ARG(impl_trait_def(impl_def)->name), buf);
    diag_note(rctx->diags, (Span){0}, "the other one is in module '" SV_FMT "'",
              SV_ARG(other->module->file_path));
    break;
  }
  impl_index_add(rctx->impls, impl_def);
  impl_index_add(&rctx->tc->all_impls, impl_def);

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
      fun_def->impl = impl_def;
      fun_def->body = fun_decl->body;
      fun_def->span = item->span;
      fun_def->slot = FUN_SLOT_NONE;

      // resolve method type parameters
      fun_def->type_param_count = fun_decl->type_param_count;
      fun_def->type_params = al_alloc_zero(
          rctx->al, sizeof(StringView) * fun_decl->type_param_count);
      for (int j = 0; j < fun_def->type_param_count; j++) {
        fun_def->type_params[j] = resolve_generic_param(
            rctx, &fun_decl->type_params[j], fun_decl->where_clause);
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

static Type *trait_project(Type *t, TraitDef *trait, Type *self_to,
                           ImplDef *impl, Allocator *al);

// The `Self` a trait's default *bodies* are written against: a real type
// parameter bounded by the trait, not the abstract trait type. Interned like
// any other generic, so every mention of it — the body's signature, the type
// scope it is checked under, the key an instantiation is compiled under — is
// the same type.
static Type *trait_self_param(TraitDef *trait, Allocator *al) {
  TraitDef *bounds[1] = {trait};
  return ty_generic(sv_from_cstr("Self"), bounds, 1, al);
}

// Give a default body a definition of its own, so it can be compiled like the
// generic function it effectively is:
//
//     trait Show { fun twice(self) -> Int { self.show() + self.show() } }
//     ⇒  fun twice<Self: Show>(self: Self) -> Int { ... }
//
// The trait's `method_type` states `Self` as the trait type, which a name-keyed
// `Subst` cannot bind — so the signature is projected onto the type parameter
// once, here. Calls on `self` inside the body then dispatch through the bound
// exactly as they do in a `<T: Show>` function, which is machinery codegen
// already has.
static FunDef *resolve_trait_default_impl(ResolveCtx *rctx, TraitDef *trait_def,
                                          TraitMethodDef *method_def,
                                          TraitItemNode *item) {
  Type *self_param = trait_self_param(trait_def, rctx->al);

  FunDef *fun = al_alloc_zero_for(rctx->al, FunDef);
  fun->name = item->name;
  fun->module = trait_def->module;
  fun->body = item->default_body;
  fun->span = item->span;
  fun->slot = FUN_SLOT_NONE;

  // `Self` first, then the method's own parameters — the order cg_inst_key
  // walks them in does not matter (the key is name-addressed), but a body can
  // mention both.
  fun->type_param_count = method_def->type_param_count + 1;
  fun->type_params =
      al_alloc(rctx->al, sizeof(Type *) * (size_t)fun->type_param_count);
  fun->type_params[0] = self_param;
  for (int i = 0; i < method_def->type_param_count; i++) {
    fun->type_params[i + 1] = method_def->type_params[i];
  }

  Type *fun_ty = trait_project(method_def->method_type, trait_def, self_param,
                               /*impl=*/NULL, rctx->al);
  fun->fun_type = fun_ty;
  fun->return_type = fun_ty->as.fun.return_type;

  fun->param_count = item->param_count;
  fun->params =
      al_alloc_zero(rctx->al, sizeof(ParamDef) * (size_t)item->param_count);
  for (int i = 0; i < item->param_count; i++) {
    fun->params[i].name =
        item->params[i].is_self ? sv_from_cstr("self") : item->params[i].name;
    fun->params[i].is_self = item->params[i].is_self;
    fun->params[i].param_type = fun_ty->as.fun.param_types[i];
  }
  return fun;
}

static void resolve_trait_decl(ResolveCtx *rctx, Decl *decl) {
  assert(decl->kind == DECL_TRAIT && "expected trait decl");
  DeclTrait *trait_decl = &decl->as.trait_decl;
  TraitDef *trait_def = trait_decl->def;

  Type *trait_ty = ty_trait(trait_def, rctx->al);
  trait_def->self_type = trait_ty;

  // define the trait name up front so its own item signatures (and, in source
  // order, later impls) can reference it.
  TypeEntry *te = NULL;
  tscope_define(rctx->tyres.tscope, trait_def->name, trait_ty, rctx->diags,
                decl->span, &te);
  te->as.trait_def = trait_def;

  // count items by kind and allocate the def tables.
  for (int i = 0; i < trait_decl->item_count; i++) {
    if (trait_decl->items[i].kind == TRAIT_ITEM_METHOD) {
      trait_def->method_count++;
    } else {
      trait_def->assoc_type_count++;
    }
  }
  trait_def->methods =
      al_alloc_zero(rctx->al, sizeof(TraitMethodDef) * trait_def->method_count);
  trait_def->assoc_types = al_alloc_zero(
      rctx->al, sizeof(AssocTypeDef) * trait_def->assoc_type_count);

  // begin; trait level type scope
  rctx->tyres.tscope = tscope_push(rctx->tyres.tscope, rctx->al);

  // `Self` inside the trait is the abstract trait type; `Self.Assoc`
  // projections stay abstract until an impl binds them (see TYNODE_ASSOC).
  tscope_define(rctx->tyres.tscope, sv_from_cstr("Self"), trait_ty, rctx->diags,
                decl->span, NULL);

  // record associated-type names first so method signatures can project
  // `Self.Assoc`. The concrete type is supplied per-impl, so it stays NULL.
  for (int i = 0, assoc_idx = 0; i < trait_decl->item_count; i++) {
    TraitItemNode *item = &trait_decl->items[i];
    if (item->kind != TRAIT_ITEM_ASSOC_TYPE) {
      continue;
    }
    trait_def->assoc_types[assoc_idx].name = item->name;
    trait_def->assoc_types[assoc_idx].type = NULL;
    assoc_idx++;
  }

  // resolve method signatures (bodies of default methods are not checked yet).
  for (int i = 0, method_idx = 0; i < trait_decl->item_count; i++) {
    TraitItemNode *item = &trait_decl->items[i];
    if (item->kind != TRAIT_ITEM_METHOD) {
      continue;
    }
    TraitMethodDef *method_def = &trait_def->methods[method_idx++];
    method_def->name = item->name;
    method_def->has_default = item->default_body != NULL;

    // method type parameters
    method_def->type_param_count = item->type_param_count;
    method_def->type_params =
        al_alloc_zero(rctx->al, sizeof(Type *) * item->type_param_count);
    for (int j = 0; j < item->type_param_count; j++) {
      method_def->type_params[j] =
          resolve_generic_param(rctx, &item->type_params[j], NULL);
    }

    // begin; method level type scope
    rctx->tyres.tscope = tscope_push(rctx->tyres.tscope, rctx->al);
    for (int j = 0; j < item->type_param_count; j++) {
      tscope_define(rctx->tyres.tscope, item->type_params[j].name,
                    method_def->type_params[j], rctx->diags,
                    item->type_params[j].span, NULL);
    }

    // resolve parameters (self resolves to the abstract trait type) and return.
    TypeScratch param_types;
    ts_init(&param_types, item->param_count, rctx->al);
    method_def->self_index = -1;
    for (int j = 0; j < item->param_count; j++) {
      if (item->params[j].is_self) {
        method_def->self_index = j;
        param_types.ptr[j] = trait_ty;
      } else {
        param_types.ptr[j] =
            rctx_resolve(rctx, item->params[j].type_annotation);
      }
    }
    Type *return_type = item->return_type
                            ? rctx_resolve(rctx, item->return_type)
                            : rctx->tc->t_unit;

    // end; method level type scope
    rctx->tyres.tscope = tscope_pop(rctx->tyres.tscope);

    method_def->method_type =
        ty_fun(param_types.ptr, param_types.count, return_type, rctx->al);

    // a default body is a definition of its own; its *body* is checked in
    // pass 3 (tc_check_trait), like any other function's.
    if (item->default_body != NULL) {
      method_def->default_impl =
          resolve_trait_default_impl(rctx, trait_def, method_def, item);
    }
  }

  // end; trait level type scope
  rctx->tyres.tscope = tscope_pop(rctx->tyres.tscope);
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
  case DECL_VAR:
  case DECL_POISON:
    break; // already diagnosed during registration
  }
}

bool tc_resolve_module(TypeChecker *tc, Module *m) {
  ResolveCtx rctx;
  rctx_init(&rctx, tc, m, tc->diags, tc->al);

  for (int i = 0; i < m->ast->decl_count; i++) {
    Decl *decl = m->ast->decls[i];
    rctx.tyres.tscope = &m->tscope;
    resolve_decl(&rctx, decl);
  }

  return true;
}

static Type *resolve_expr(CheckCtx *ctx, Expr *expr, Type *hint);
static bool check_coerce_dyn(CheckCtx *ctx, Expr *e, Type *actual,
                             Type *expected);
static Type *trait_project(Type *t, TraitDef *trait, Type *self_to,
                           ImplDef *impl, Allocator *al);
static void check_binding_pattern(CheckCtx *ctx, Pattern *pat, Type *init_ty);

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
      if (check_coerce_dyn(ctx, var->initializer, init_ty, annot_ty)) {
        init_ty = annot_ty;
      } else {
        init_ty =
            infer_unify(&ctx->infer, annot_ty, init_ty, ctx->diags, stmt->span)
                ? annot_ty // prefer annotation on success
                : ctx->tc->t_poison;
      }
    }

    check_binding_pattern(ctx, var->binding, init_ty);
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

    if (!type_is_poison(val_ty) && !type_is_poison(ctx->return_type) &&
        !check_coerce_dyn(ctx, ret->value, val_ty, ctx->return_type)) {
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
    callee->as.path_expr.resolved_fun = def;

    if (def->type_param_count == 0) {
      // the impl-level subst may map its generics to fresh unknowns (bare
      // generic self like `Point::new`); pass it up so arguments are unified
      // rather than compared strictly. it was already applied into r.type.
      *subst = r.as.method.subst;
      cctx_record_inst(ctx, &callee->as.path_expr.inst, *subst);
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

    // the instantiation covers the impl's type params too: `r.type` already
    // has the impl subst applied, so `*subst` alone need not, but the body
    // codegen compiles still mentions them.
    Subst impl_subst = subst_exclude_shadowed(
        r.as.method.subst, def->type_params, def->type_param_count, ctx->al);
    cctx_record_inst(ctx, &callee->as.path_expr.inst,
                     subst_concat(impl_subst, *subst, ctx->al));

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

// ═══════════════════════════════════════════════════════════════════════════════
// Coercion to a trait object
// ═══════════════════════════════════════════════════════════════════════════════

// The language has no subtyping: type identity is pointer equality and
// `infer_unify` only ever decomposes structurally. A trait object is the one
// place that is not enough — `Sq` and `dyn Shape` are genuinely different
// runtime representations, and the conversion between them is a real
// operation (allocate the fat value, attach the vtable), not a re-reading of
// the same bits.
//
// So it is modelled as exactly that: an explicit, one-way, non-transitive
// coercion, attempted only where a value flows into a *known* `dyn Trait`
// position and never as part of unification. The check is `impl_index_
// implements`, the same question a trait bound asks — a trait object is a
// bound whose witness is carried at runtime instead of resolved at compile
// time.
//
// The result is recorded on the expression rather than folded into its type,
// so the node keeps saying what it *is* and codegen learns what to wrap.
// Returns true when `expected` was satisfied by coercing `e`.
static bool check_coerce_dyn(CheckCtx *ctx, Expr *e, Type *actual,
                             Type *expected) {
  if (e == NULL || expected == NULL || expected->kind != TY_DYN) {
    return false;
  }
  actual = infer_apply(&ctx->infer, actual, ctx->al);
  switch (actual->kind) {
  case TY_DYN:     // already one
  case TY_UNKNOWN: // undecidable here; unification may still solve it
  case TY_POISON:  // already reported
  case TY_TRAIT:   // the abstract `Self` of a default body — known gap
  case TY_ASSOC:
    return false;
  default:
    // TY_GENERIC is deliberately allowed: `impl_index_implements` answers a
    // bounded `T` from its own declared bounds, and codegen substitutes the
    // concrete type before building the vtable — so a generic function can
    // hand its parameter over as a trait object.
    break;
  }

  TraitDef *trait = expected->as.dyn.def;
  if (!impl_index_implements(ctx->impls, actual, trait, ctx->al)) {
    return false;
  }

  e->coerce_dyn = trait;
  return true;
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

    if (check_coerce_dyn(ctx, call->args[i], arg_ty, param_ty)) {
      continue;
    }

    if (is_generic) {
      had_error |= !infer_unify(&ctx->infer, param_ty, arg_ty, ctx->diags,
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
  pattern->as.struc.resolved_struct = struct_def;

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

  // the path names a struct, but not necessarily *this* one. Field types are
  // read off `struct_def` while the value has `expected_ty`'s layout, so
  // without this every same-shaped struct is silently interchangeable and a
  // field comes back with the wrong type entirely. `check_variant_pattern`
  // has always made the matching check for enums.
  if (expected_ty->as.struc.def != struct_def) {
    char et[64];
    type_sprintf(expected_ty, et, sizeof(et));
    diag_error(ctx->diags, pattern->span,
               "struct pattern '" SV_FMT "' does not match expected type '%s'",
               SV_ARG(struct_def->name), et);
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
      sub_pattern_error |= !check_pattern(ctx, fp->sub_pattern, field_ty);
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
  pattern->as.variant.resolved_variant = variant_def;

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
      sub_pattern_error |= !check_pattern(ctx, fp->sub_pattern, field_ty);
    } else {
      assert(!variant_def->is_tuple &&
             "tuple variant patterns must have sub-patterns");
      vscope_define(ctx->vscope, fp->ident.name, field_ty, ctx->diags, fp->span,
                    NULL);
    }
  }

  for (int i = check_n; i < got_argc; i++) {
    sub_pattern_error |= !check_pattern(
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
    // mirrors the expression side: a bare unit struct name is a constructor,
    // not a binding, so `match x { Marker => ... }` tests for `Marker`
    // instead of silently matching everything under that name.
    TypeEntry *te = tscope_lookup(ctx->tscope, pattern->as.bind.name);
    if (te && te->type->kind == TY_STRUCT &&
        te->as.struct_def->field_count == 0) {
      PathSegment *seg = al_alloc_zero(ctx->al, sizeof(PathSegment));
      seg->name = pattern->as.bind.name;
      Pattern new = {
          .kind = PAT_STRUCT,
          .span = pattern->span,
          .resolved_type = expected_ty,
          .as.struc =
              {
                  .path = {.segments = seg, .count = 1, .span = pattern->span},
                  .field_count = 0,
                  .fields = NULL,
              },
      };
      *pattern = new;
      return check_struct_pattern(ctx, pattern, expected_ty, te->as.struct_def);
    }

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
      // `*pattern = new` replaces the whole node, so resolved_type has to be
      // carried across: exhaustiveness reads the column type off it, and a
      // NULL there reads as "unconstrained" — which made every tuple-struct
      // pattern look like a coverage gap.
      Pattern new = {
          .kind = PAT_STRUCT,
          .span = pattern->span,
          .resolved_type = expected_ty,
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
          .resolved_type = expected_ty, // see the mirrored rewrite above
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
      sub_pattern_error |= !check_pattern(ctx, elem_pat, elem_ty);
    }

    if (sub_pattern_error) {
      return false;
    }
    break;
  }
  }

  return true;
}

// ═══════════════════════════════════════════════════════════════════════════════
// Match exhaustiveness
// ═══════════════════════════════════════════════════════════════════════════════
//
// Maranget's usefulness algorithm, specialised to the single question we ask of
// a match: is the all-wildcard row still useful against the arms' patterns? If
// it is, some value reaches no arm. The matrix holds one row per arm, one
// column per value still to be discriminated; a NULL column is a wildcard — a
// `_`, a binding, or a field the pattern simply omitted.
//
// The answer is deliberately tri-state. A column whose type inference has not
// pinned down (an unsolved unknown, a generic) has an unknowable domain, so
// rather than guess we report nothing: a false "not exhaustive" would reject a
// correct program, which is worse than the OP_MATCH_FAIL backstop the VM
// already has.

// the widest signature we enumerate: an enum with more variants than this is
// treated as unenumerable, so it needs a `_` arm rather than a wrong answer.
#define MAX_SIGNATURE 256

typedef enum { EXH_YES, EXH_NO, EXH_UNKNOWN } Exhaustive;

typedef enum {
  CT_NONE,    // irrefutable: `_` or a binding
  CT_BOOL,    // a `true` / `false` literal
  CT_LIT,     // any other literal — one point of an unenumerable domain
  CT_VARIANT, // one variant of an enum
  CT_SINGLE,  // the sole constructor of a struct or tuple
  CT_OPAQUE,  // resolved to nothing we can reason about
} CtorKind;

typedef struct {
  CtorKind kind;
  int arity;
  VariantDef *variant; // CT_VARIANT
  bool bool_value;     // CT_BOOL
} Ctor;

typedef struct {
  Pattern **cols;
} PatRow;

typedef struct {
  PatRow *rows;
  int row_count;
  int width;
} PatMatrix;

// the constructor a pattern tests for. A variant pattern that lists no fields
// (`Opt::Some`) still names its variant — it is refutable, it just constrains
// none of the payload.
static Ctor pat_ctor(Pattern *p) {
  if (p == NULL) {
    return (Ctor){.kind = CT_NONE};
  }

  switch (p->kind) {
  case PAT_WILDCARD:
  case PAT_BIND:
    return (Ctor){.kind = CT_NONE};
  case PAT_LITERAL:
    if (p->as.literal_expr->kind == EXPR_BOOL) {
      return (Ctor){.kind = CT_BOOL,
                    .bool_value = p->as.literal_expr->as.bool_val};
    }
    return (Ctor){.kind = CT_LIT};
  case PAT_VARIANT: {
    VariantDef *v = p->as.variant.resolved_variant;
    if (v == NULL) {
      return (Ctor){.kind = CT_OPAQUE};
    }
    return (Ctor){.kind = CT_VARIANT, .arity = v->field_count, .variant = v};
  }
  case PAT_STRUCT: {
    StructDef *s = p->as.struc.resolved_struct;
    if (s == NULL) {
      return (Ctor){.kind = CT_OPAQUE};
    }
    return (Ctor){.kind = CT_SINGLE, .arity = s->field_count};
  }
  case PAT_TUPLE:
    return (Ctor){.kind = CT_SINGLE, .arity = p->as.tuple.count};
  }
  return (Ctor){.kind = CT_OPAQUE};
}

static bool ctor_matches(Ctor a, Ctor b) {
  if (a.kind != b.kind) {
    return false;
  }
  switch (a.kind) {
  case CT_BOOL:
    return a.bool_value == b.bool_value;
  case CT_VARIANT:
    return a.variant == b.variant;
  case CT_SINGLE:
    return true;
  default:
    return false; // CT_LIT values are compared by nothing we can see here
  }
}

// the sub-pattern for field `k` of a struct/variant constructor: patterns may
// list fields out of order, omit them entirely, or use the `{ x }` shorthand —
// all of which are wildcards at position k.
static Pattern *field_pat_at(FieldPat *fields, int field_count,
                             const FieldDef *defs, bool is_tuple, int k) {
  for (int i = 0; i < field_count; i++) {
    bool hit = is_tuple ? fields[i].ident.index == k
                        : sv_equal(fields[i].ident.name, defs[k].ident.name);
    if (hit) {
      return fields[i].sub_pattern; // NULL for shorthand, i.e. a binding
    }
  }
  return NULL;
}

// write `ctor`'s sub-patterns of `p` into `out[0..arity)`.
static void ctor_sub_patterns(Pattern *p, Ctor ctor, Pattern **out) {
  switch (p->kind) {
  case PAT_TUPLE:
    for (int i = 0; i < ctor.arity; i++) {
      out[i] = p->as.tuple.elems[i];
    }
    return;
  case PAT_STRUCT: {
    StructDef *s = p->as.struc.resolved_struct;
    for (int i = 0; i < ctor.arity; i++) {
      out[i] = field_pat_at(p->as.struc.fields, p->as.struc.field_count,
                            s->fields, s->is_tuple, i);
    }
    return;
  }
  case PAT_VARIANT: {
    VariantDef *v = p->as.variant.resolved_variant;
    for (int i = 0; i < ctor.arity; i++) {
      out[i] = field_pat_at(p->as.variant.fields, p->as.variant.field_count,
                            v->fields, v->is_tuple, i);
    }
    return;
  }
  default:
    return; // arity 0
  }
}

// S(ctor, m): rows whose head is `ctor` (payload spread over `arity` new
// columns) or a wildcard (`arity` fresh wildcards), with column 0 consumed.
static PatMatrix matrix_specialize(const PatMatrix *m, Ctor ctor,
                                   Allocator *al) {
  PatMatrix out = {.width = ctor.arity + m->width - 1};
  out.rows = al_alloc_zero(al, sizeof(PatRow) * m->row_count);

  for (int i = 0; i < m->row_count; i++) {
    Pattern *head = m->rows[i].cols[0];
    Ctor head_ctor = pat_ctor(head);
    if (head_ctor.kind != CT_NONE && !ctor_matches(head_ctor, ctor)) {
      continue;
    }

    Pattern **cols =
        al_alloc_zero(al, sizeof(Pattern *) * (out.width > 0 ? out.width : 1));
    if (head_ctor.kind != CT_NONE) {
      ctor_sub_patterns(head, ctor, cols);
    }
    for (int c = 1; c < m->width; c++) {
      cols[ctor.arity + c - 1] = m->rows[i].cols[c];
    }
    out.rows[out.row_count++] = (PatRow){.cols = cols};
  }
  return out;
}

// D(m): rows whose head is a wildcard, with column 0 dropped.
static PatMatrix matrix_default(const PatMatrix *m, Allocator *al) {
  PatMatrix out = {.width = m->width - 1};
  out.rows = al_alloc_zero(al, sizeof(PatRow) * m->row_count);

  for (int i = 0; i < m->row_count; i++) {
    if (pat_ctor(m->rows[i].cols[0]).kind != CT_NONE) {
      continue;
    }
    out.rows[out.row_count++] = (PatRow){.cols = m->rows[i].cols + 1};
  }
  return out;
}

// the type of column 0, taken from the first row that constrains it. A column
// nothing constrains needs no type: every row is a wildcard there, so the
// default matrix is the whole matrix and the domain never matters.
static Type *column_type(CheckCtx *ctx, const PatMatrix *m) {
  for (int i = 0; i < m->row_count; i++) {
    Pattern *p = m->rows[i].cols[0];
    if (p != NULL && p->resolved_type != NULL) {
      return infer_apply(&ctx->infer, p->resolved_type, ctx->al);
    }
  }
  return NULL;
}

// the constructors that between them cover every value of `ty`, or false if the
// domain is unenumerable (Int, String, ...) or unknown.
static bool type_signature(Type *ty, Ctor *sig, int *count) {
  if (ty == NULL) {
    return false;
  }

  switch (ty->kind) {
  case TY_BOOL:
    sig[0] = (Ctor){.kind = CT_BOOL, .bool_value = false};
    sig[1] = (Ctor){.kind = CT_BOOL, .bool_value = true};
    *count = 2;
    return true;
  case TY_ENUM: {
    EnumDef *def = ty->as.enm.def;
    if (def->variant_count > MAX_SIGNATURE) {
      return false;
    }
    for (int i = 0; i < def->variant_count; i++) {
      sig[i] = (Ctor){.kind = CT_VARIANT,
                      .arity = def->variants[i].field_count,
                      .variant = &def->variants[i]};
    }
    *count = def->variant_count;
    return true;
  }
  case TY_STRUCT:
    sig[0] = (Ctor){.kind = CT_SINGLE, .arity = ty->as.struc.def->field_count};
    *count = 1;
    return true;
  case TY_TUPLE:
    sig[0] = (Ctor){.kind = CT_SINGLE, .arity = ty->as.tuple.elem_count};
    *count = 1;
    return true;
  default:
    return false;
  }
}

// a type whose domain we cannot enumerate is still *certain* if we know we
// cannot: Int has infinitely many values, so a wildcard is genuinely required.
// An unsolved unknown is a different thing — we simply do not know yet.
static bool type_domain_is_certain(Type *ty) {
  if (ty == NULL) {
    return true; // nothing constrains the column; a wildcard is required
  }
  switch (ty->kind) {
  case TY_UNKNOWN:
  case TY_GENERIC:
  case TY_ASSOC:
  case TY_TRAIT:
  case TY_POISON:
    return false;
  default:
    return true;
  }
}

static Exhaustive matrix_covers(CheckCtx *ctx, const PatMatrix *m) {
  if (m->width == 0) {
    return m->row_count > 0 ? EXH_YES : EXH_NO;
  }

  Type *ty = column_type(ctx, m);

  Ctor sig[MAX_SIGNATURE];
  int sig_count = 0;
  if (type_signature(ty, sig, &sig_count)) {
    // the signature is only usable if the arms actually mention every one of
    // its constructors; otherwise the missing ones fall through to D(m).
    bool complete = true;
    for (int c = 0; c < sig_count && complete; c++) {
      complete = false;
      for (int i = 0; i < m->row_count && !complete; i++) {
        complete = ctor_matches(pat_ctor(m->rows[i].cols[0]), sig[c]);
      }
    }

    if (complete) {
      for (int c = 0; c < sig_count; c++) {
        PatMatrix s = matrix_specialize(m, sig[c], ctx->al);
        Exhaustive r = matrix_covers(ctx, &s);
        if (r != EXH_YES) {
          return r;
        }
      }
      return EXH_YES;
    }
  }

  PatMatrix d = matrix_default(m, ctx->al);
  if (d.row_count == 0) {
    return type_domain_is_certain(ty) ? EXH_NO : EXH_UNKNOWN;
  }
  return matrix_covers(ctx, &d);
}

// name a top-level constructor no arm mentions, for the diagnostic. Only the
// first column can be described this cheaply; deeper gaps get the generic
// message, which is still true.
static bool missing_top_ctor(const PatMatrix *m, Type *ty, char *buf,
                             size_t buf_size) {
  if (ty == NULL || ty->kind != TY_ENUM) {
    return false;
  }

  EnumDef *def = ty->as.enm.def;
  for (int v = 0; v < def->variant_count; v++) {
    Ctor want = {.kind = CT_VARIANT, .variant = &def->variants[v]};
    bool seen = false;
    for (int i = 0; i < m->row_count && !seen; i++) {
      Ctor head = pat_ctor(m->rows[i].cols[0]);
      seen = head.kind == CT_NONE || ctor_matches(head, want);
    }
    if (!seen) {
      snprintf(buf, buf_size, SV_FMT "::" SV_FMT, SV_ARG(def->name),
               SV_ARG(def->variants[v].name));
      return true;
    }
  }
  return false;
}

// a guarded arm is not counted: whether it matches is a runtime question, so it
// can never be what makes a match exhaustive.
static void check_match_exhaustive(CheckCtx *ctx, Expr *expr,
                                   Type *subject_ty) {
  int arm_count = expr->as.match.arm_count;

  PatMatrix m = {.width = 1};
  m.rows =
      al_alloc_zero(ctx->al, sizeof(PatRow) * (arm_count > 0 ? arm_count : 1));
  for (int i = 0; i < arm_count; i++) {
    MatchArm *arm = &expr->as.match.arms[i];
    if (arm->guard != NULL) {
      continue;
    }
    Pattern **cols = al_alloc_zero(ctx->al, sizeof(Pattern *));
    cols[0] = arm->pattern;
    m.rows[m.row_count++] = (PatRow){.cols = cols};
  }

  if (matrix_covers(ctx, &m) != EXH_NO) {
    return;
  }

  char ctor[128];
  if (missing_top_ctor(&m, infer_apply(&ctx->infer, subject_ty, ctx->al), ctor,
                       sizeof(ctor))) {
    diag_error(ctx->diags, expr->span,
               "match is not exhaustive: '%s' is not covered", ctor);
  } else {
    diag_error(ctx->diags, expr->span,
               "match is not exhaustive: add a '_' arm to cover the "
               "remaining cases");
  }
}

// define every name `pat` binds as poison. Reached when the pattern could not
// be checked against a real type: the names are still in scope as far as the
// programmer is concerned, so binding them suppresses a cascade of
// "undefined variable" errors on top of the one real diagnostic.
static void bind_pattern_poison(CheckCtx *ctx, Pattern *pat) {
  if (pat == NULL) {
    return;
  }

  switch (pat->kind) {
  case PAT_WILDCARD:
  case PAT_LITERAL:
    break;
  case PAT_BIND:
    vscope_define(ctx->vscope, pat->as.bind.name, ctx->tc->t_poison, ctx->diags,
                  pat->span, NULL);
    break;
  case PAT_TUPLE:
    for (int i = 0; i < pat->as.tuple.count; i++) {
      bind_pattern_poison(ctx, pat->as.tuple.elems[i]);
    }
    break;
  case PAT_STRUCT:
  case PAT_VARIANT: {
    FieldPat *fields =
        pat->kind == PAT_STRUCT ? pat->as.struc.fields : pat->as.variant.fields;
    int count = pat->kind == PAT_STRUCT ? pat->as.struc.field_count
                                        : pat->as.variant.field_count;
    for (int i = 0; i < count; i++) {
      if (fields[i].sub_pattern != NULL) {
        bind_pattern_poison(ctx, fields[i].sub_pattern);
      } else {
        vscope_define(ctx->vscope, fields[i].ident.name, ctx->tc->t_poison,
                      ctx->diags, fields[i].span, NULL);
      }
    }
    break;
  }
  }
}

// a `var` binding is a match with one arm and no guard, so "irrefutable" is
// exactly "exhaustive" over a one-row matrix — the same question
// check_match_exhaustive asks, and the same tri-state answer: a column whose
// type inference has not pinned down reports nothing rather than guessing.
static void check_binding_pattern(CheckCtx *ctx, Pattern *pat, Type *init_ty) {
  if (type_is_poison(init_ty)) {
    bind_pattern_poison(ctx, pat);
    return;
  }

  if (!check_pattern(ctx, pat, init_ty)) {
    // check_pattern reported the mismatch and stopped where it found it, so
    // some of the names below that point were never defined.
    bind_pattern_poison(ctx, pat);
    return;
  }

  Pattern **cols = al_alloc_zero(ctx->al, sizeof(Pattern *));
  cols[0] = pat;
  PatRow row = {.cols = cols};
  PatMatrix m = {.rows = &row, .row_count = 1, .width = 1};

  if (matrix_covers(ctx, &m) == EXH_NO) {
    diag_error(ctx->diags, pat->span,
               "refutable pattern in a 'var' binding: it does not cover every "
               "value of the initializer; use 'match' instead");
  }
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

  // only once every pattern checked: exhaustiveness reads resolved_type and the
  // resolved struct/variant defs off the patterns.
  check_match_exhaustive(ctx, expr, subject_ty);

  return result_ty ? result_ty : ctx->tc->t_unit;
}

// Check `obj.m(args)` against a *trait's* method signature rather than an
// impl method — either because the receiver is abstract (a bounded type
// parameter, or `Self` in a default body), or because the receiver's impl
// inherited the method's default body without overriding it.
//
// The signature is written in the trait's terms, so it is rewritten in two
// steps: `impl` (NULL for an abstract receiver) resolves `Self.Assoc` against
// what that impl bound, and `impl_subst` then replaces the impl's own type
// params with the receiver's type arguments.
static Type *check_trait_method_call(CheckCtx *ctx, Expr *expr, TraitDef *trait,
                                     TraitMethodDef *tm, Type *self_ty,
                                     ImplDef *impl, Subst impl_subst) {
  ExprMethodCall *mc = &expr->as.method_call;

  if (tm->self_index < 0) {
    diag_error(ctx->diags, expr->span,
               "'" SV_FMT "' is an associated function, not a method — it has "
               "no 'self' parameter",
               SV_ARG(mc->method_name));
    return ctx->tc->t_poison;
  }

  if (mc->type_arg_count > 0 && mc->type_arg_count != tm->type_param_count) {
    diag_error(ctx->diags, expr->span, "expected %d type arguments but got %d",
               tm->type_param_count, mc->type_arg_count);
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

  // the method's own type params first (subst_apply matches TY_GENERIC by
  // name), only then `Self` — otherwise a method type param sharing the
  // receiver parameter's name would capture the just-projected receiver.
  Subst subst =
      infer_open_generics(&ctx->infer, tm->type_params, explicit_type_args.ptr,
                          tm->type_param_count, expr->span, ctx->al);
  bool is_generic = subst.count > 0;

  if (impl == NULL) {
    // dispatch through a bound: which impl supplies the body depends on what
    // `self_ty` is instantiated with, so codegen redoes the lookup once it
    // knows — and it may land on an impl method or on the trait's default.
    mc->bound_trait = trait;
    mc->bound_self = self_ty;
  }

  // record `Self` alongside the method's own type args: it is the first type
  // parameter of the default body, the one definition a call can reach whose
  // signature is written in terms of the receiver rather than of an impl. An
  // impl method has no parameter of that name, so the extra binding is simply
  // unused when the call lands on one.
  cctx_record_inst(ctx, &mc->inst, subst_with_self(subst, self_ty, ctx->al));

  Type *fun_ty = subst_apply(&subst, tm->method_type, ctx->al);
  fun_ty = trait_project(fun_ty, trait, self_ty, impl, ctx->al);

  impl_subst = subst_exclude_shadowed(impl_subst, tm->type_params,
                                      tm->type_param_count, ctx->al);
  if (impl_subst.count > 0) {
    fun_ty = subst_apply(&impl_subst, fun_ty, ctx->al);
  }

  int expected_argc = fun_ty->as.fun.param_count - 1;
  if (mc->arg_count != expected_argc) {
    diag_error(ctx->diags, expr->span, "expected %d arguments but got %d",
               expected_argc, mc->arg_count);
    return ctx->tc->t_poison;
  }

  bool had_error = false;
  for (int i = 0, k = 0; i < fun_ty->as.fun.param_count; i++) {
    if (i == tm->self_index) {
      continue;
    }
    Type *param_ty = fun_ty->as.fun.param_types[i];
    Expr *arg = mc->args[k++];
    Type *arg_ty = resolve_expr(ctx, arg, param_ty);

    if (type_is_poison(arg_ty) || type_is_poison(param_ty)) {
      had_error = true;
      continue;
    }

    if (check_coerce_dyn(ctx, arg, arg_ty, param_ty)) {
      continue;
    }

    if (is_generic) {
      had_error |=
          !infer_unify(&ctx->infer, param_ty, arg_ty, ctx->diags, arg->span);
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
  return is_generic ? infer_apply(&ctx->infer, ret_ty, ctx->al) : ret_ty;
}

// Dispatch `obj.m(args)` on an abstract receiver through its trait bounds: a
// bounded type parameter offers its bounds, `Self` in a default body offers
// the trait itself. Returns NULL when no bound declares the name, so the
// caller can fall back to the impl index (a blanket `impl<T> Trait for T` can
// still apply). First bound declaring the name wins, like impl selection.
static Type *resolve_bound_method_call(CheckCtx *ctx, Expr *expr, Type *self_ty,
                                       TraitDef **bounds, int bound_count) {
  StringView name = expr->as.method_call.method_name;

  for (int i = 0; i < bound_count; i++) {
    for (int j = 0; j < bounds[i]->method_count; j++) {
      if (sv_equal(bounds[i]->methods[j].name, name)) {
        return check_trait_method_call(ctx, expr, bounds[i],
                                       &bounds[i]->methods[j], self_ty,
                                       /*impl=*/NULL, subst_empty());
      }
    }
  }
  return NULL;
}

// The body of a method call, given a receiver whose type is already known.
// Split out for interpolation, which has to resolve the receiver *first* (to
// decide whether it is a primitive at all) and must not resolve it twice —
// a second pass would re-report its diagnostics and re-queue its
// instantiations.
static Type *resolve_method_call_typed(CheckCtx *ctx, Expr *expr,
                                       Type *self_ty) {
  ExprMethodCall *mc = &expr->as.method_call;

  // an abstract receiver dispatches through its trait bounds instead of the
  // impl index: a type parameter offers the bounds it was declared with, and
  // `Self` inside a trait default body offers that trait's own signatures.
  TraitDef *self_trait[1];
  TraitDef **bounds = NULL;
  int bound_count = 0;
  if (self_ty->kind == TY_GENERIC) {
    bounds = self_ty->as.generic.bounds;
    bound_count = self_ty->as.generic.bound_count;
  } else if (self_ty->kind == TY_TRAIT) {
    self_trait[0] = self_ty->as.trait.def;
    bounds = self_trait;
    bound_count = 1;
  } else if (self_ty->kind == TY_DYN) {
    // a trait object offers exactly the one trait it names. It checks like
    // any other abstract receiver — object safety is what guarantees the
    // signature stays callable once `Self` is gone — and only *codegen* tells
    // the two apart, by the vtable.
    self_trait[0] = self_ty->as.dyn.def;
    bounds = self_trait;
    bound_count = 1;
  }
  if (bound_count > 0) {
    Type *bound_ty =
        resolve_bound_method_call(ctx, expr, self_ty, bounds, bound_count);
    if (bound_ty != NULL) {
      return bound_ty;
    }
  }

  ImplMatch match;
  MethodDef *method =
      impl_index_method(ctx->impls, self_ty, mc->method_name, &match,
                        &ctx->infer, /*bare_path=*/false, expr->span, ctx->al);
  if (!method) {
    // no impl defines it — the receiver may still inherit a trait's default
    // body, which is checked against the trait's signature.
    ImplDef *via_impl = NULL;
    TraitDef *via_trait = NULL;
    Subst via_subst = subst_empty();
    TraitMethodDef *inherited =
        impl_index_default_method(ctx->impls, self_ty, mc->method_name,
                                  &via_impl, &via_trait, &via_subst, ctx->al);
    if (inherited != NULL) {
      mc->resolved_impl = via_impl;
      mc->resolved_default = inherited->default_impl;
      return check_trait_method_call(ctx, expr, via_trait, inherited, self_ty,
                                     via_impl, via_subst);
    }

    char self_buf[64];
    type_sprintf(self_ty, self_buf, sizeof(self_buf));
    diag_error(ctx->diags, expr->span,
               "no method named '" SV_FMT "' found for type '%s'",
               SV_ARG(mc->method_name), self_buf);
    ImplQuery q = {.self_type = self_ty,
                   .method = mc->method_name,
                   .impls = ctx->impls,
                   .al = ctx->al};
    note_unimported_impl(
        ctx->diags,
        find_unimported_impl(ctx->tc, ctx->impls, impl_query_matches, &q));
    note_blocking_bound(ctx->diags, ctx->impls, self_ty, NULL, mc->method_name,
                        ctx->al);
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

  // combine the impl-level substitution (from matching self_ty) with the
  // method's own generics (fresh unknowns / explicit type args) into a single
  // subst, so subst_apply sees every generic that can appear in fun->fun_type
  // in one pass — and so the instantiation recorded for codegen binds all of
  // them, which is what compiling the body needs.
  Subst method_subst =
      infer_open_generics(&ctx->infer, fun->type_params, explicit_type_args.ptr,
                          fun->type_param_count, expr->span, ctx->al);
  bool is_generic = method_subst.count > 0;

  Subst impl_subst = subst_exclude_shadowed(match.subst, fun->type_params,
                                            fun->type_param_count, ctx->al);

  Subst subst = subst_concat(impl_subst, method_subst, ctx->al);
  cctx_record_inst(ctx, &mc->inst, subst);

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

    if (check_coerce_dyn(ctx, arg, arg_ty, param_ty)) {
      continue;
    }

    if (is_generic) {
      had_error |=
          !infer_unify(&ctx->infer, param_ty, arg_ty, ctx->diags, arg->span);
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

static Type *resolve_method_call_expr(CheckCtx *ctx, Expr *expr, Type *hint) {
  (void)hint;
  Type *self_ty = resolve_expr(ctx, expr->as.method_call.object, NULL);
  if (type_is_poison(self_ty)) {
    return ctx->tc->t_poison;
  }
  return resolve_method_call_typed(ctx, expr, self_ty);
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
  prop->ok_variant = op_ok;
  prop->err_variant = op_err;

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
  def->slot = FUN_SLOT_NONE; // closures are built by OP_CLOSURE, not addressed
  def->body = closure->body;
  def->span = expr->span;
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

// A generic constructor written without type arguments takes them from the
// expected type, when the hint names the same struct or enum: `Opt::None` has
// no field to solve `T` from, and neither does a bare `Wrap`. Seeding only —
// a hint that disagrees with the fields is still a mismatch, reported where
// the value is used.
static Type **hint_type_args(Type *hint, Type *self_type) {
  if (hint == NULL || hint == self_type) {
    return NULL;
  }
  if (hint->kind == TY_ENUM && self_type->kind == TY_ENUM &&
      hint->as.enm.def == self_type->as.enm.def) {
    return hint->as.enm.type_args;
  }
  if (hint->kind == TY_STRUCT && self_type->kind == TY_STRUCT &&
      hint->as.struc.def == self_type->as.struc.def) {
    return hint->as.struc.type_args;
  }
  return NULL;
}

// ═══════════════════════════════════════════════════════════════════════════════
// Interpolation
// ═══════════════════════════════════════════════════════════════════════════════
//
// A `"{v}"` segment has to turn `v` into a String. The primitives render
// themselves: the VM's `stringify` has always known how, and keeping that path
// is what lets a program interpolate an `Int` without importing anything.
//
// Everything else asks the *type* how it wants to render, through `std::fmt`'s
// `Display`. Once the trait is what decides, the segment *is* the call
// `v.to_string()` — so that is exactly what it is rewritten into, and the
// ordinary machinery does the rest. Dispatch through a trait bound, through a
// `dyn`, an inherited default body, and monomorphising the instance all arrive
// already working; codegen, `OP_INTERP`, the VM and the image format need no
// change at all, because the segment now simply evaluates to a String, which
// `stringify` passes straight through.
//
// Note that `print` is *not* held to this: it renders any value structurally,
// through `value_print`. The two are different questions — "show me what this
// is" is a debugging view the runtime can always answer, while "render this
// for a reader" is the type's own decision, and only the second needs a trait.

// does `type` satisfy `Display`? The impl index answers for a concrete type
// and for a bounded generic; the two abstract kinds it has no entry for
// answer from the trait they name.
static bool display_satisfied(CheckCtx *ctx, Type *type) {
  TraitDef *display = ctx->tc->display_trait;
  if (type->kind == TY_DYN) {
    return type->as.dyn.def == display;
  }
  if (type->kind == TY_TRAIT) {
    // the abstract `Self` of a default body, which offers its own trait only
    return type->as.trait.def == display;
  }
  return impl_index_implements(ctx->impls, type, display, ctx->al);
}

static void check_interpol_seg(CheckCtx *ctx, InterpolSeg *seg) {
  Expr *recv = seg->expr;
  Type *seg_ty = resolve_expr(ctx, recv, NULL);
  seg_ty = infer_apply(&ctx->infer, seg_ty, ctx->al);
  if (type_is_poison(seg_ty)) {
    return; // already reported
  }

  switch (seg_ty->kind) {
  case TY_INT:
  case TY_FLOAT:
  case TY_BOOL:
  case TY_STRING:
    return; // rendered by the VM; no call is emitted at all
  default:
    break;
  }

  char buf[64];
  type_sprintf(seg_ty, buf, sizeof(buf));

  if (seg_ty->kind == TY_UNKNOWN) {
    diag_error(ctx->diags, recv->span,
               "cannot infer the type of this interpolated value");
    return;
  }
  if (ctx->tc->display_trait == NULL) {
    diag_error(ctx->diags, recv->span,
               "cannot interpolate a value of type '%s': only a primitive "
               "renders itself, and this program does not import 'std::fmt' "
               "to get the 'Display' trait",
               buf);
    return;
  }
  if (!display_satisfied(ctx, seg_ty)) {
    if (seg_ty->kind == TY_GENERIC) {
      // a type parameter satisfies exactly what it was declared to, so the
      // fix is at the declaration rather than at an impl
      diag_error(ctx->diags, recv->span,
                 "cannot interpolate a value of type '%s': add the bound "
                 "'%s: Display'",
                 buf, buf);
      return;
    }
    diag_error(ctx->diags, recv->span,
               "cannot interpolate a value of type '%s': it does not "
               "implement 'Display'",
               buf);
    ImplQuery q = {.self_type = seg_ty,
                   .trait = ctx->tc->display_trait,
                   .impls = ctx->impls,
                   .al = ctx->al};
    note_unimported_impl(
        ctx->diags,
        find_unimported_impl(ctx->tc, ctx->impls, impl_query_matches, &q));
    note_blocking_bound(ctx->diags, ctx->impls, seg_ty, ctx->tc->display_trait,
                        (StringView){0}, ctx->al);
    return;
  }

  Expr *call = al_alloc_zero_for(ctx->al, Expr);
  *call = (Expr){
      .kind = EXPR_METHOD_CALL,
      .span = recv->span,
      .as.method_call = {.object = recv,
                         .method_name = sv_from_cstr("to_string")},
  };
  seg->expr = call;

  // the receiver is already resolved, so this must not go through
  // resolve_method_call_expr. A failure here is a malformed `Display` impl,
  // which conformance checking has already reported against the impl itself.
  resolve_method_call_typed(ctx, call, seg_ty);
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
      // the only valid multi-segment path expression (without a call or
      // struct-init suffix) is a qualified unit variant, e.g. `Status::Off`.
      PathRes r;
      if (!cctx_resolve_path(ctx, &expr->as.path_expr.path, &r)) {
        result = ctx->tc->t_poison;
        break;
      }

      if (r.kind != PATHRES_VARIANT) {
        diag_error(ctx->diags, expr->span,
                   "unexpected multi-segment path expression without a call");
        result = ctx->tc->t_poison;
        break;
      }

      if (r.as.variant.def->field_count != 0) {
        diag_error(ctx->diags, expr->span,
                   "variant '" SV_FMT "' requires arguments",
                   SV_ARG(r.as.variant.def->name));
        result = ctx->tc->t_poison;
        break;
      }

      rewrite_tuple_variant_call(ctx, expr, r.type, r.as.variant.enum_def,
                                 r.as.variant.def);
      // re-resolve as a variant initializer, exactly as the call and
      // struct-init spellings do. `r.type` is the *declared* enum type, so
      // returning it directly would hand back the abstract `Opt<T>`; the
      // EXPR_VARIANT case opens the parameters into fresh unknowns the hint
      // can solve.
      return resolve_expr(ctx, expr, hint);
    }

    StringView name =
        expr->as.path_expr.path.segments[expr->as.path_expr.path.count - 1]
            .name;

    bool crossed_fn = false;
    VarEntry *ve = vscope_lookup(ctx->vscope, name, &crossed_fn);
    if (!ve) {
      // a bare unit struct names a value as well as a type: `struct Unit;`
      // then `var u = Unit;`. Consulted only after the value scope, so a
      // binding of that name would still win — though the mirrored rewrite in
      // `check_pattern` means one can no longer be created.
      TypeEntry *te = tscope_lookup(ctx->tscope, name);
      if (te && te->type->kind == TY_STRUCT &&
          te->as.struct_def->field_count == 0) {
        // the struct-init path resolves the path itself, so type arguments
        // and the hint are handled the same as for `Unit {}`.
        ExprStructInit init = {
            .path = expr->as.path_expr.path,
            .fields = NULL,
            .field_count = 0,
            .resolved_struct = NULL,
        };
        *expr = (Expr){
            .kind = EXPR_STRUCT_INIT,
            .span = expr->span,
            .as.struct_init = init,
        };
        return resolve_expr(ctx, expr, hint);
      }

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
      // record which function the name meant, for codegen: the binding may
      // come from another module, or from an alias, so a name search over the
      // enclosing module's own funs would not find it. NULL for a local of
      // function type (a closure, a parameter) — those are stack slots.
      expr->as.path_expr.resolved_fun = def;

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
      cctx_record_inst(ctx, &expr->as.path_expr.inst, subst);

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
      Type **args = had_explicit_args ? struct_ty->as.struc.type_args
                                      : hint_type_args(hint, def->self_type);
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

      if (check_coerce_dyn(ctx, field_init->value, arg_ty, field_ty)) {
        continue;
      }

      if (!infer_unify(&ctx->infer, field_ty, arg_ty, ctx->diags,
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
      Type **args = had_explicit_args
                        ? resolved_enum_ty->as.enm.type_args
                        : hint_type_args(hint, enum_def->self_type);
      subst =
          infer_open_generics(&ctx->infer, enum_def->type_params, args,
                              enum_def->type_param_count, expr->span, ctx->al);
    }

    // check fields
    bool had_error = false;
    for (int i = 0; i < init->field_count; i++) {
      FieldInit *field_init = &init->fields[i];

      FieldDef *field_def = find_variant_field(variant_def, field_init->ident,
                                               ctx->diags, expr->span);
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

      if (check_coerce_dyn(ctx, field_init->value, arg_ty, field_ty)) {
        continue;
      }

      if (!infer_unify(&ctx->infer, field_ty, arg_ty, ctx->diags,
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
        field->resolved_index = (int)(field_def - def->fields);
      }

      if (is_generic) {
        result = subst_apply(&subst, result, ctx->al);
        // result = infer_apply(&ctx->infer, result, ctx->al);
      }

      break;
    }

    if (base_ty->kind == TY_TUPLE) {
      if (!field->is_tuple) {
        diag_error(ctx->diags, expr->span,
                   "tuple fields must be accessed with '.0', '.1', ...");
        result = ctx->tc->t_poison;
        break;
      }

      int idx = field->ident.index;
      if (idx < 0 || idx >= base_ty->as.tuple.elem_count) {
        diag_error(ctx->diags, expr->span,
                   "tuple index %d out of bounds for tuple of size %d", idx,
                   base_ty->as.tuple.elem_count);
        result = ctx->tc->t_poison;
        break;
      }

      field->resolved_index = idx;
      result = base_ty->as.tuple.elem_types[idx];
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

    // an element-wise hint, so a `(dyn Shape, Int)` annotation reaches the
    // element that has to coerce. Only used when the shape already matches;
    // a mismatched hint is left for the surrounding unification to report.
    Type *hint_ty = hint ? infer_find(&ctx->infer, hint) : NULL;
    if (hint_ty != NULL && (hint_ty->kind != TY_TUPLE ||
                            hint_ty->as.tuple.elem_count != tuple->count)) {
      hint_ty = NULL;
    }

    bool had_error = false;
    for (int i = 0; i < tuple->count; i++) {
      Type *elem_hint = hint_ty ? hint_ty->as.tuple.elem_types[i] : NULL;
      elem_types.ptr[i] = resolve_expr(ctx, tuple->elems[i], elem_hint);
      if (type_is_poison(elem_types.ptr[i])) {
        had_error = true;
        continue;
      }
      if (elem_hint != NULL && check_coerce_dyn(ctx, tuple->elems[i],
                                                elem_types.ptr[i], elem_hint)) {
        elem_types.ptr[i] = elem_hint;
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

    // `[dyn Shape]` is the shape trait objects exist for: a heterogeneous
    // array. The hint decides the element type up front and every element
    // coerces into it, rather than the first element deciding and the rest
    // having to match it.
    if (elem_hint != NULL && elem_hint->kind == TY_DYN) {
      bool ok = true;
      for (int i = 0; i < array->count; i++) {
        Type *ty = resolve_expr(ctx, array->elems[i], elem_hint);
        if (type_is_poison(ty)) {
          ok = false;
          continue;
        }
        if (check_coerce_dyn(ctx, array->elems[i], ty, elem_hint)) {
          continue;
        }
        ok &= infer_unify(&ctx->infer, elem_hint, ty, ctx->diags,
                          array->elems[i]->span);
      }
      result = ok ? ty_array(elem_hint, ctx->al) : ctx->tc->t_poison;
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
      if (seg->kind == ISEG_EXPR) {
        check_interpol_seg(ctx, seg);
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

  // a native's body is in C, so its signature is the whole declaration and
  // there is nothing here to check. (A malformed `@native` already reported
  // at registration and left `body` NULL too.)
  if (fun_decl->body == NULL) {
    return;
  }

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
  infer_check_bounds(&cctx.infer, tc, cctx.diags, cctx.al);
  cctx_solve_insts(&cctx);
}

// Rewrite a trait method's signature type into the terms of a concrete impl:
// the abstract `Self` (the trait type) becomes the impl's self type, and each
// `Self.Assoc` projection becomes the concrete type the impl bound to it. Used
// to compare a trait's required signature against what an impl actually wrote.
// `impl` may be NULL — then there is no binding to substitute and `Self.Assoc`
// is merely rebased onto `self_to`, which is what a call through a trait bound
// (`T: Iterator` → `t.next()`) needs: the projection stays abstract as
// `T.Item`.
static Type *trait_project(Type *t, TraitDef *trait, Type *self_to,
                           ImplDef *impl, Allocator *al) {
  switch (t->kind) {
  case TY_TRAIT:
    return t->as.trait.def == trait ? self_to : t;
  case TY_ASSOC:
    if (t->as.assoc.base->kind == TY_TRAIT &&
        t->as.assoc.base->as.trait.def == trait) {
      if (impl == NULL) {
        // no impl to read the binding from (a call through a bound): the
        // projection stays abstract, just rebased onto the new self.
        return ty_assoc(self_to, t->as.assoc.assoc_name, trait, al);
      }
      for (int i = 0; i < impl->assoc_type_count; i++) {
        if (sv_equal(impl->assoc_types[i].name, t->as.assoc.assoc_name)) {
          return impl->assoc_types[i].type;
        }
      }
    }
    return t;
  case TY_FUNCTION: {
    TypeScratch params;
    ts_init(&params, t->as.fun.param_count, al);
    for (int i = 0; i < t->as.fun.param_count; i++) {
      params.ptr[i] =
          trait_project(t->as.fun.param_types[i], trait, self_to, impl, al);
    }
    Type *ret = trait_project(t->as.fun.return_type, trait, self_to, impl, al);
    return ty_fun(params.ptr, params.count, ret, al);
  }
  case TY_TUPLE: {
    TypeScratch elems;
    ts_init(&elems, t->as.tuple.elem_count, al);
    for (int i = 0; i < t->as.tuple.elem_count; i++) {
      elems.ptr[i] =
          trait_project(t->as.tuple.elem_types[i], trait, self_to, impl, al);
    }
    return ty_tuple(elems.ptr, elems.count, al);
  }
  case TY_ARRAY:
    return ty_array(
        trait_project(t->as.array.elem_type, trait, self_to, impl, al), al);
  case TY_STRUCT: {
    TypeScratch args;
    ts_init(&args, t->as.struc.type_arg_count, al);
    for (int i = 0; i < t->as.struc.type_arg_count; i++) {
      args.ptr[i] =
          trait_project(t->as.struc.type_args[i], trait, self_to, impl, al);
    }
    return ty_struct(t->as.struc.def, args.ptr, args.count, al);
  }
  case TY_ENUM: {
    TypeScratch args;
    ts_init(&args, t->as.enm.type_arg_count, al);
    for (int i = 0; i < t->as.enm.type_arg_count; i++) {
      args.ptr[i] =
          trait_project(t->as.enm.type_args[i], trait, self_to, impl, al);
    }
    return ty_enum(t->as.enm.def, args.ptr, args.count, al);
  }
  default:
    return t;
  }
}

// Verify a `impl Trait for T` provides everything the trait requires: each
// associated type, and each required method with a matching signature. Methods
// with a default body may be omitted. Extra (inherent) methods are tolerated.
static void tc_check_impl_conformance(TypeChecker *tc, Decl *decl,
                                      ImplDef *impl_def) {
  if (impl_def->trait_type == NULL || impl_def->trait_type->kind != TY_TRAIT) {
    return; // inherent impl, or a poisoned trait head already diagnosed
  }
  TraitDef *trait = impl_def->trait_type->as.trait.def;

  for (int i = 0; i < trait->assoc_type_count; i++) {
    StringView name = trait->assoc_types[i].name;
    bool found = false;
    for (int j = 0; j < impl_def->assoc_type_count; j++) {
      if (sv_equal(impl_def->assoc_types[j].name, name)) {
        found = true;
        break;
      }
    }
    if (!found) {
      diag_error(tc->diags, decl->span,
                 "impl of trait '" SV_FMT
                 "' is missing associated type '" SV_FMT "'",
                 SV_ARG(trait->name), SV_ARG(name));
    }
  }

  for (int i = 0; i < trait->method_count; i++) {
    TraitMethodDef *tm = &trait->methods[i];
    MethodDef *impl_method = NULL;
    for (int j = 0; j < impl_def->method_count; j++) {
      if (sv_equal(impl_def->methods[j].name, tm->name)) {
        impl_method = &impl_def->methods[j];
        break;
      }
    }

    if (impl_method == NULL) {
      if (!tm->has_default) {
        diag_error(tc->diags, decl->span,
                   "impl of trait '" SV_FMT "' is missing method '" SV_FMT "'",
                   SV_ARG(trait->name), SV_ARG(tm->name));
      }
      continue;
    }

    Type *expected = trait_project(tm->method_type, trait, impl_def->self_type,
                                   impl_def, tc->al);
    Type *actual = impl_method->fun->fun_type;
    if (!type_is_poison(expected) && !type_is_poison(actual) &&
        !types_equal(expected, actual)) {
      char want[64], got[64];
      type_sprintf(expected, want, sizeof(want));
      type_sprintf(actual, got, sizeof(got));
      diag_error(tc->diags, decl->span,
                 "method '" SV_FMT "' does not match trait '" SV_FMT
                 "': expected '%s', found '%s'",
                 SV_ARG(tm->name), SV_ARG(trait->name), want, got);
    }
  }
}

static void tc_check_impl(TypeChecker *tc, Decl *decl) {
  assert(decl->kind == DECL_IMPL && "expected impl decl");
  DeclImpl *impl_decl = &decl->as.impl_decl;
  ImplDef *impl_def = impl_decl->def;

  tc_check_impl_conformance(tc, decl, impl_def);

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

  infer_finalize(&cctx.infer, cctx.diags);
  infer_check_bounds(&cctx.infer, tc, cctx.diags, cctx.al);
  cctx_solve_insts(&cctx);
}

// Check the bodies of a trait's default methods, once — not once per impl.
// `self` has the body's own `Self` type parameter, bounded by the trait, so
// `self.other()` dispatches through the trait's own signatures
// (resolve_bound_method_call) and `Self.Assoc` stays an unbound projection: a
// body that checks here is valid for every impl that could inherit it.
static void tc_check_trait(TypeChecker *tc, Decl *decl) {
  assert(decl->kind == DECL_TRAIT && "expected trait decl");
  DeclTrait *trait_decl = &decl->as.trait_decl;
  TraitDef *trait_def = trait_decl->def;

  CheckCtx cctx;
  cctx_init(&cctx, tc, trait_def->module, tc->diags, tc->al);

  // begin trait type scope. Only bodies are checked here, and a body is
  // written against the `Self` type parameter its own definition introduced —
  // the abstract trait type never appears in one.
  cctx.tyres.tscope = tscope_push(cctx.tyres.tscope, cctx.al);
  tscope_define(cctx.tyres.tscope, sv_from_cstr("Self"),
                trait_self_param(trait_def, cctx.al), cctx.diags, decl->span,
                NULL);

  for (int i = 0, method_idx = 0; i < trait_decl->item_count; i++) {
    TraitItemNode *item = &trait_decl->items[i];
    if (item->kind != TRAIT_ITEM_METHOD) {
      continue;
    }
    TraitMethodDef *method_def = &trait_def->methods[method_idx++];
    FunDef *fun = method_def->default_impl;
    if (fun == NULL) {
      continue; // required method: a signature only
    }

    cctx.fun = fun;
    cctx.return_type = fun->return_type;

    // begin method type scope. `Self` is fun->type_params[0], already in scope
    // from the trait level; the method's own follow it.
    cctx.tyres.tscope = tscope_push(cctx.tyres.tscope, cctx.al);
    for (int j = 0; j < item->type_param_count; j++) {
      tscope_define(cctx.tyres.tscope, item->type_params[j].name,
                    fun->type_params[j + 1], cctx.diags,
                    item->type_params[j].span, NULL);
    }

    // begin method var scope
    cctx.vscope = vscope_push(cctx.vscope, true, false, cctx.al);
    for (int j = 0; j < fun->param_count; j++) {
      vscope_define(cctx.vscope, fun->params[j].name, fun->params[j].param_type,
                    cctx.diags, item->params[j].span, NULL);
    }

    resolve_expr_coerced(&cctx, fun->body, cctx.return_type);

    // end method var scope
    cctx.vscope = vscope_pop(cctx.vscope);
    // end method type scope
    cctx.tyres.tscope = tscope_pop(cctx.tyres.tscope);
  }

  // end trait type scope
  cctx.tyres.tscope = tscope_pop(cctx.tyres.tscope);

  infer_finalize(&cctx.infer, cctx.diags);
  infer_check_bounds(&cctx.infer, tc, cctx.diags, cctx.al);
  cctx_solve_insts(&cctx);
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
    case DECL_USE:
    case DECL_VAR:
    case DECL_POISON:
      break;
    case DECL_TRAIT:
      tc_check_trait(tc, decl);
      break;
    case DECL_IMPL:
      tc_check_impl(tc, decl);
      break;
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

// does `t` mention `trait`'s abstract `Self`, either directly or under a
// projection (`Self.Item`)? Used for object safety: such a type is only
// meaningful once `Self` is a *known* concrete type, which is exactly what a
// trait object has thrown away.
static bool type_mentions_self(const Type *t, const TraitDef *trait) {
  switch (t->kind) {
  case TY_TRAIT:
    return t->as.trait.def == trait;
  case TY_ASSOC:
    return t->as.assoc.trait == trait ||
           type_mentions_self(t->as.assoc.base, trait);
  case TY_FUNCTION:
    for (int i = 0; i < t->as.fun.param_count; i++) {
      if (type_mentions_self(t->as.fun.param_types[i], trait)) {
        return true;
      }
    }
    return type_mentions_self(t->as.fun.return_type, trait);
  case TY_TUPLE:
    for (int i = 0; i < t->as.tuple.elem_count; i++) {
      if (type_mentions_self(t->as.tuple.elem_types[i], trait)) {
        return true;
      }
    }
    return false;
  case TY_ARRAY:
    return type_mentions_self(t->as.array.elem_type, trait);
  case TY_STRUCT:
    for (int i = 0; i < t->as.struc.type_arg_count; i++) {
      if (type_mentions_self(t->as.struc.type_args[i], trait)) {
        return true;
      }
    }
    return false;
  case TY_ENUM:
    for (int i = 0; i < t->as.enm.type_arg_count; i++) {
      if (type_mentions_self(t->as.enm.type_args[i], trait)) {
        return true;
      }
    }
    return false;
  default:
    return false;
  }
}

// Object safety. A `dyn Trait` value is one vtable slot per method, called
// with the receiver as its own first argument and nothing else known about
// it — so a method belongs in a vtable only if that is enough to call it:
//
//   • it must take `self` (an associated function has no receiver to dispatch
//     on, so there is no vtable to find it in);
//   • it must have no type parameters of its own (each instantiation would
//     need a slot, and which ones exist isn't known at the coercion site);
//   • `Self` must not appear anywhere but the receiver — a `-> Self` or a
//     `other: Self` parameter means the *caller* has to know the concrete
//     type, which is precisely what the coercion erased. `Self.Item` is the
//     same problem behind a projection.
//
// Checked where `dyn Trait` is written rather than at the trait declaration:
// a trait is free to be statically-dispatch-only (`tests/run/trait_default.dt`
// leans on every one of these), and only naming it as a type asks for more.
static bool trait_check_object_safe(TypeResolver *r, TraitDef *trait,
                                    Span span) {
  for (int i = 0; i < trait->method_count; i++) {
    TraitMethodDef *m = &trait->methods[i];
    const char *why = NULL;

    if (m->self_index < 0) {
      why = "it has no 'self' parameter";
    } else if (m->type_param_count > 0) {
      why = "it has type parameters of its own";
    } else if (m->method_type != NULL &&
               type_mentions_self(m->method_type, trait)) {
      // the receiver is `Self` by construction; look past it.
      Type *ft = m->method_type;
      bool elsewhere = type_mentions_self(ft->as.fun.return_type, trait);
      for (int p = 0; !elsewhere && p < ft->as.fun.param_count; p++) {
        if (p == m->self_index) {
          continue;
        }
        elsewhere = type_mentions_self(ft->as.fun.param_types[p], trait);
      }
      if (elsewhere) {
        why = "'Self' appears in its signature outside the receiver";
      }
    }

    if (why != NULL) {
      diag_error(r->diags, span,
                 "trait '" SV_FMT "' is not object-safe: method '" SV_FMT
                 "' cannot be called through 'dyn " SV_FMT "' because %s",
                 SV_ARG(trait->name), SV_ARG(m->name), SV_ARG(trait->name),
                 why);
      return false;
    }
  }
  return true;
}

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
      } else if (sv_equal_cstr(name, "Never")) {
        // the type of code that does not come back. It already existed as the
        // type `return`/`break` give an expression; naming it is what lets a
        // signature promise divergence, which is the whole of `panic`'s
        // contract — `infer_unify` lets it stand in for any type, so
        // `return panic(msg)` satisfies any return type.
        result = r->tc->t_never;
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
  case TYNODE_DYN: {
    TypeNodeNamed *dyn = &node->as.dyn;

    if (dyn->path.count != 1) {
      diag_error(r->diags, node->span,
                 "a qualified path cannot name a trait object — import the "
                 "trait with 'use' and write 'dyn <Trait>'");
      result = r->tc->t_poison;
      break;
    }

    StringView name = dyn->path.segments[0].name;
    TypeEntry *e = tscope_lookup(r->tscope, name);
    if (e == NULL || e->type == NULL || e->type->kind != TY_TRAIT) {
      diag_error(r->diags, node->span,
                 "'dyn' needs a trait, but '" SV_FMT "' is not one",
                 SV_ARG(name));
      result = r->tc->t_poison;
      break;
    }

    TraitDef *trait = e->type->as.trait.def;
    if (!trait_check_object_safe(r, trait, node->span)) {
      result = r->tc->t_poison;
      break;
    }

    result = ty_dyn(trait, r->al);
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

    // `T.Assoc` on a type parameter: the projection is only meaningful if one
    // of T's bounds declares that associated type. It stays abstract (like
    // `Self.Assoc` below) — instantiation substitutes T, and the concrete
    // binding is read off the impl then.
    if (base->kind == TY_GENERIC) {
      TraitDef *owner = NULL;
      for (int i = 0; i < base->as.generic.bound_count && !owner; i++) {
        TraitDef *trait_def = base->as.generic.bounds[i];
        for (int j = 0; j < trait_def->assoc_type_count; j++) {
          if (sv_equal(trait_def->assoc_types[j].name, assoc->assoc_name)) {
            owner = trait_def;
            break;
          }
        }
      }
      if (owner == NULL) {
        char buf[64];
        type_sprintf(base, buf, sizeof(buf));
        diag_error(r->diags, node->span,
                   "no trait bound on '%s' declares an associated type '" SV_FMT
                   "'",
                   buf, SV_ARG(assoc->assoc_name));
        result = r->tc->t_poison;
        break;
      }
      result = ty_assoc(base, assoc->assoc_name, owner, r->al);
      break;
    }

    // `Self.Assoc` inside a trait declaration: the base is the trait's own
    // abstract self type, so the projection stays abstract (a TY_ASSOC) until
    // an impl binds it. Only names the trait actually declares are valid.
    if (base->kind == TY_TRAIT) {
      TraitDef *trait_def = base->as.trait.def;
      for (int i = 0; i < trait_def->assoc_type_count; i++) {
        if (sv_equal(trait_def->assoc_types[i].name, assoc->assoc_name)) {
          result = ty_assoc(base, assoc->assoc_name, trait_def, r->al);
          break;
        }
      }
      if (result != NULL) {
        break;
      }
      char buf[64];
      type_sprintf(base, buf, sizeof(buf));
      diag_error(r->diags, node->span,
                 "type '%s' has no associated type '" SV_FMT "'", buf,
                 SV_ARG(assoc->assoc_name));
      result = r->tc->t_poison;
      break;
    }

    result = impl_index_assoc_type(r->impls, base, assoc->assoc_name, r->al);
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
  for (int i = 0; i < idx->count; i++) {
    if (idx->all[i] == impl) {
      return; // already reachable by another path — a diamond import
    }
  }
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

    Subst subst;
    if (!impl_applies(impl, self_type, &subst, NULL, al)) {
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

// Does an impl's own type params satisfy their declared bounds, once the
// receiver has pinned them down? `impl<T: Display> Display for [T]` applies to
// `[Int]` but not to `[NoShow]`, and asking here — at selection — is what keeps
// the failure at the call site. Left unasked, the impl is selected regardless
// and the bound is only felt inside its body, where the diagnostic names the
// impl's source rather than the code that reached for it.
//
// Answering recurses: `[[Int]]` asks whether `[Int]` is Display, which selects
// this same impl one level down. The type shrinks each step, so the only way
// not to terminate is a self-referential blanket impl
// (`impl<T: Foo> Foo for T`), which the depth cap catches.
#define IMPL_BOUND_MAX_DEPTH 32
static int impl_bound_depth = 0;

static bool impl_bounds_satisfied(ImplDef *impl, Type **args, int n,
                                  ImplIndex *idx, Allocator *al) {
  if (impl_bound_depth >= IMPL_BOUND_MAX_DEPTH) {
    return false;
  }
  impl_bound_depth++;
  bool ok = true;
  for (int j = 0; ok && j < n; j++) {
    Type *param = impl->type_params[j];
    if (param == NULL || param->kind != TY_GENERIC) {
      continue;
    }
    for (int b = 0; ok && b < param->as.generic.bound_count; b++) {
      ok = impl_index_implements(idx, args[j], param->as.generic.bounds[b], al);
    }
  }
  impl_bound_depth--;
  return ok;
}

// Does `impl` apply to a receiver of type `self_type`? For a non-generic impl
// that's plain type identity; for a generic one, the impl's self type is
// matched structurally against the receiver and *out_subst is filled with the
// resulting binding of the impl's type params (every one of which must be
// pinned down by the receiver, or the impl doesn't apply).
//
// `bounds_idx` is the visible set to answer the impl's own bounds against, or
// NULL to match structurally only. Coherence (`impl_defs_conflict`) and
// associated-type lookup pass NULL: both run while the index is still filling,
// and coherence wants the conservative answer anyway — two impls for one type
// overlap whether or not their bounds happen to be disjoint.
static bool impl_applies(ImplDef *impl, Type *self_type, Subst *out_subst,
                         ImplIndex *bounds_idx, Allocator *al) {
  *out_subst = subst_empty();
  if (impl->self_type == NULL || type_is_poison(impl->self_type)) {
    return false;
  }

  if (impl->type_param_count == 0) {
    return types_equal(impl->self_type, self_type);
  }

  int n = impl->type_param_count;
  StringView *names = al_alloc(al, sizeof(StringView) * n);
  Type **args = al_alloc(al, sizeof(Type *) * n);
  bool *bound = al_alloc_zero(al, sizeof(bool) * n);
  for (int j = 0; j < n; j++) {
    names[j] = impl->type_params[j]->as.generic.name;
  }

  bool matched =
      impl_type_match(impl->self_type, self_type, names, n, args, bound);
  for (int j = 0; matched && j < n; j++) {
    matched = bound[j];
  }
  if (matched && bounds_idx != NULL) {
    matched = impl_bounds_satisfied(impl, args, n, bounds_idx, al);
  }
  if (matched) {
    subst_init(out_subst, names, args, n);
  }
  return matched;
}

// The trait an impl heads, or NULL for an inherent impl.
static TraitDef *impl_trait_def(ImplDef *impl) {
  if (impl->trait_type == NULL || impl->trait_type->kind != TY_TRAIT) {
    return NULL;
  }
  return impl->trait_type->as.trait.def;
}

bool impl_defs_conflict(ImplDef *a, ImplDef *b, Allocator *al) {
  TraitDef *trait = impl_trait_def(a);
  if (trait == NULL || trait != impl_trait_def(b)) {
    return false;
  }
  if (a->self_type == NULL || b->self_type == NULL ||
      type_is_poison(a->self_type) || type_is_poison(b->self_type)) {
    return false; // already diagnosed; don't pile on
  }

  // Overlap is asked the same way selection asks it, from both sides: does
  // either impl apply to the other's self type? For two non-generic impls
  // that is plain identity; for `Ord for Option<T>` against
  // `Ord for Option<Int>` the generic one matches the concrete one's self
  // type, which is the case a receiver would find ambiguous.
  Subst ignored;
  return impl_applies(a, b->self_type, &ignored, NULL, al) ||
         impl_applies(b, a->self_type, &ignored, NULL, al);
}

// A trait method the receiver inherits rather than defines: some
// `impl Trait for <self_type>` exists, the trait declares `name` with a default
// body, and the impl didn't override it (if it had, impl_index_method would
// have found it first). *out_impl / *out_trait / *out_subst describe the impl
// the default is inherited through, so the caller can project the trait's
// signature into concrete terms.
TraitMethodDef *impl_index_default_method(ImplIndex *idx, Type *self_type,
                                          StringView name, ImplDef **out_impl,
                                          TraitDef **out_trait,
                                          Subst *out_subst, Allocator *al) {
  if (type_is_poison(self_type)) {
    return NULL;
  }
  for (int i = 0; i < idx->count; i++) {
    ImplDef *impl = idx->all[i];
    TraitDef *trait = impl_trait_def(impl);
    if (trait == NULL) {
      continue;
    }
    Subst subst;
    if (!impl_applies(impl, self_type, &subst, idx, al)) {
      continue;
    }
    for (int j = 0; j < trait->method_count; j++) {
      if (!trait->methods[j].has_default ||
          !sv_equal(trait->methods[j].name, name)) {
        continue;
      }
      *out_impl = impl;
      *out_trait = trait;
      *out_subst = subst;
      return &trait->methods[j];
    }
  }
  return NULL;
}

MethodDef *impl_index_method(ImplIndex *idx, Type *self_type, StringView name,
                             ImplMatch *out_match, InferCtx *infer,
                             bool bare_path, Span span, Allocator *al) {
  if (type_is_poison(self_type)) {
    return NULL;
  }

  // bare generic self: select the impl by method name and open its type
  // params as fresh unknowns — argument unification at the call site pins
  // down the type arguments (e.g. `Point::new(10, 20)` selecting
  // `impl Point<Int>`). first name match wins.
  // Only a bare path (`Point::new`) may select an impl by method name. A
  // method-call receiver never may: since TY_GENERIC is interned, a generic
  // impl's `Self` is pointer-identical to the struct's canonical self type,
  // so type_is_bare_generic_self alone can no longer tell the two apart.
  if (infer != NULL && bare_path && type_is_bare_generic_self(self_type)) {
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
    Subst subst;
    if (!impl_applies(impl, self_type, &subst, idx, al)) {
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

// Does `type` implement `trait`? True if any registered impl heads
// `impl [<..>] trait for T` with a self type matching `type` — exact for a
// non-generic impl, structural (binding the impl's params) for a generic one.
bool impl_index_implements(ImplIndex *idx, Type *type, TraitDef *trait,
                           Allocator *al) {
  if (type_is_poison(type)) {
    return true; // already diagnosed; don't pile on
  }
  if (type->kind == TY_GENERIC) {
    // a type parameter has no impls of its own; what it satisfies is exactly
    // what it was declared to. This is what lets one bounded generic hand its
    // parameter to another (`fun a<T: Show>(v: T) { b(v) }`), and it is sound
    // because the bound is re-checked against the concrete type wherever the
    // outer function is instantiated.
    for (int i = 0; i < type->as.generic.bound_count; i++) {
      if (type->as.generic.bounds[i] == trait) {
        return true;
      }
    }
    return false;
  }
  for (int i = 0; i < idx->count; i++) {
    ImplDef *impl = idx->all[i];
    if (impl_trait_def(impl) != trait) {
      continue;
    }
    Subst subst;
    if (impl_applies(impl, type, &subst, idx, al)) {
      return true;
    }
  }
  return false;
}

// ═══════════════════════════════════════════════════════════════════════════════
// ResolveCtx
// ═══════════════════════════════════════════════════════════════════════════════

void rctx_init(ResolveCtx *rctx, TypeChecker *tc, Module *m, DiagBag *diags,
               Allocator *al) {
  memset(rctx, 0, sizeof(*rctx));

  rctx->tc = tc;
  rctx->m = m;
  rctx->impls = &m->visible_impls;
  rctx->diags = diags;
  rctx->al = al;

  rctx->tyres = (TypeResolver){
      .tc = tc,
      .tscope = NULL,
      .impls = &m->visible_impls,
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
  infer_init(&cctx->infer, &m->visible_impls, al);

  cctx->tc = tc;
  cctx->impls = &m->visible_impls;
  cctx->diags = diags;
  cctx->al = al;

  cctx->tyres = (TypeResolver){
      .tc = tc,
      .tscope = NULL,
      .impls = &m->visible_impls,
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
        // a lone segment is a plain name (a function, a unit variant), not a
        // type — and since `print` stopped being ambient this is the message
        // a missing `use` produces, so it should describe the mistake rather
        // than the resolver's internals.
        if (path->count == 1) {
          diag_error(ctx->diags, path->span,
                     "cannot find '" SV_FMT "' in this scope", SV_ARG(segment));
          return false;
        }
        diag_error(ctx->diags, path->span, "unknown type '" SV_FMT "' in path",
                   SV_ARG(segment));
        if (i == 0 && path->count > 1) {
          // the shape of a module-qualified path, which isn't supported —
          // items are named directly once imported.
          diag_note(ctx->diags, (Span){0},
                    "paths are not qualified by module; import the item with "
                    "'use' and name it directly");
        }
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
      MethodDef *method = impl_index_method(
          ctx->tyres->impls, struct_ty, segment, &match, ctx->tyres->infer,
          /*bare_path=*/true, path->span, ctx->al);
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