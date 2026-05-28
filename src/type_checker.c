#include "type_checker.h"
#include "allocator.h"
#include "ast.h"
#include "string_utils.h"

#include <assert.h>
#include <stdarg.h>

// ============================================================================
// internals
// ============================================================================

typedef enum {
  SYM_STRUCT,
  SYM_FUN,
  SYM_VAR,
  SYM_ENUM,
  SYM_TYPE_PARAM,
} SymKind;

typedef struct {
  StringView name;
  SymKind kind;
  Type *type;
  int slot;

  union {
    FunDef *fun_def;
    StructDef *struct_def;
    EnumDef *enum_def;
    Type *type_param;
  } as;
} Symbol;

typedef struct SymTable {
  Symbol *entries;
  int count;
  int cap;

  int *scope_starts;
  int scope_count;
  int scope_cap;
} SymTable;

void sym_table_init(SymTable *st, Allocator *al);

void sym_table_destroy(SymTable *st, Allocator *al);

void sym_push_scope(SymTable *st, Allocator *al);

void sym_pop_scope(SymTable *st, Allocator *al);

void sym_define(SymTable *st, Symbol sym, Allocator *al);

Symbol *sym_lookup(SymTable *st, StringView name); // innermost-first

#define SUBST_MAX 8 // max type params on a single decl

typedef struct {
  StringView names[SUBST_MAX];
  Type *types[SUBST_MAX];
  int count;
} SubstEnv;

static void subst_init(SubstEnv *env);

static Type *subst_lookup(const SubstEnv *env, StringView name);

static bool subst_bind(TypeChecker *tc, SubstEnv *env, StringView name,
                       Type *concrete, Span span);

static bool unify(TypeChecker *tc, SubstEnv *env, Type *param_ty, Type *arg_ty,
                  Span span);

static Type *substitute(TypeChecker *tc, const SubstEnv *env, Type *ty);

// static bool has_unbound_generic(TypeChecker *tc, const SubstEnv *env, Type
// *ty,
//                                 Span span);

typedef enum {
  RCTX_SCOPE,  // still in the lexical symbol table
  RCTX_STRUCT, // inside a struct  → methods, associated types
  RCTX_ENUM,   // inside an enum   → variants
} ResCtxKind;

typedef struct {
  ResCtxKind kind;
  union {
    // RCTX_STRUCT
    StructDef *struct_def;
    // RCTX_ENUM
    EnumDef *enum_def;
    // RCTX_TRAIT
    TraitDef *trait_def;
  } as;
} ResCtx;

typedef enum {
  PATH_RES_UNRESOLVED,
  PATH_RES_SYMBOL,  // resolved to a top-level or local symbol
  PATH_RES_METHOD,  // Type::method → StructDef + MethodDef
  PATH_RES_VARIANT, // Enum::Variant → EnumDef + VariantDef
} PathResKind;

typedef struct {
  PathResKind kind;
  union {
    Symbol *symbol;
    struct {
      StructDef *def;
      MethodDef *method;
    } method;
    struct {
      EnumDef *def;
      VariantDef *variant;
    } variant;
  } as;
} PathRes;

static PathRes resolve_path(TypeChecker *tc, Path *path);

// static Type *path_as_type(TypeChecker *tc, Expr *expr, PathRes res);
// static Type *path_as_value(TypeChecker *tc, Expr *expr, PathRes res);

typedef struct {
  TypeChecker *tc;

  SymTable val_syms;
  SymTable type_syms;
  int loop_depth;

  FunDef *current_fun;
} TypeCheckerInternal;

#define USE_INTERNAL(_tc, name)                                                \
  TypeCheckerInternal *name = (TypeCheckerInternal *)(_tc)->internal

// ============================================================================
// internals implementation
// ============================================================================

// definition helpers

#define TYPE_ARRAY_SCRATCH_CAP 8

typedef struct {
  Type *buf[TYPE_ARRAY_SCRATCH_CAP];
  Type **ptr;
  int count;
} TypeArrayScratch;

static inline void type_array_scratch_init(TypeArrayScratch *s, int n,
                                           Allocator *al) {
  s->count = n;
  s->ptr =
      n <= TYPE_ARRAY_SCRATCH_CAP ? s->buf : al_alloc(al, n * sizeof(Type *));
  assert(s->ptr && "param_scratch_init: allocation failed");
}

static FieldDef *struct_find_field(const StructDef *def, StringView name) {
  for (int i = 0; i < def->field_count; i++) {
    if (sv_equal(def->fields[i].name, name)) {
      return &def->fields[i];
    }
  }
  return NULL;
}

static VariantDef *enum_find_variant(const EnumDef *def, StringView name) {
  for (int i = 0; i < def->variant_count; i++) {
    if (sv_equal(def->variants[i].name, name)) {
      return &def->variants[i];
    }
  }
  return NULL;
}

static void tc_error(TypeChecker *tc, Span span, const char *fmt, ...) {
  char buf[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  reporter_error(tc->reporter, span.line, span.line_end, span.col, span.col_end,
                 buf, "");
}

#define SYM_INIT_CAP 64
#define SCOPE_INIT_CAP 8

void sym_table_init(SymTable *st, Allocator *al) {
  st->entries = al_alloc(al, SYM_INIT_CAP * sizeof(Symbol));
  st->count = 0;
  st->cap = SYM_INIT_CAP;
  st->scope_starts = al_alloc(al, SCOPE_INIT_CAP * sizeof(int));
  st->scope_count = 0;
  st->scope_cap = SCOPE_INIT_CAP;
  sym_push_scope(st, al);
}

void sym_table_destroy(SymTable *st, Allocator *al) {
  al_free(al, st->entries, st->cap * sizeof(Symbol));
  al_free(al, st->scope_starts, st->scope_cap * sizeof(int));
  st->count = st->cap = 0;
  st->scope_count = st->scope_cap = 0;
}

void sym_push_scope(SymTable *st, Allocator *al) {
  if (st->scope_count == st->scope_cap) {
    int new_cap = st->scope_cap * 2;
    st->scope_starts =
        al_realloc(al, st->scope_starts, st->scope_cap * sizeof(int),
                   new_cap * sizeof(int));
    st->scope_cap = new_cap;
  }
  st->scope_starts[st->scope_count++] = st->count;
}

void sym_pop_scope(SymTable *st, Allocator *al) {
  (void)al;
  assert(st->scope_count > 0 && "sym_pop_scope: no scope to pop");
  st->count = st->scope_starts[--st->scope_count];
}

void sym_define(SymTable *st, Symbol sym, Allocator *al) {
  if (st->count == st->cap) {
    int new_cap = st->cap * 2;
    st->entries = al_realloc(al, st->entries, st->cap * sizeof(Symbol),
                             new_cap * sizeof(Symbol));
    st->cap = new_cap;
  }
  st->entries[st->count++] = sym;
}

Symbol *sym_lookup(SymTable *st, StringView name) {
  for (int i = st->count - 1; i >= 0; i--) {
    if (sv_equal(st->entries[i].name, name)) {
      return &st->entries[i];
    }
  }
  return NULL;
}
static void subst_init(SubstEnv *env) {
  *env = (SubstEnv){0};
  // env->count = 0;
}

static Type *subst_lookup(const SubstEnv *env, StringView name) {
  for (int i = 0; i < env->count; i++)
    if (sv_equal(env->names[i], name))
      return env->types[i];
  return NULL;
}

static bool subst_bind(TypeChecker *tc, SubstEnv *env, StringView name,
                       Type *concrete, Span span) {
  Type *existing = subst_lookup(env, name);
  if (existing) {
    if (existing->kind == TY_UNKNOWN) {
      return unify(tc, env, existing, concrete, span);
    }
    // already bound — must be equal
    if (!types_equal(existing, concrete)) {
      return false;
    }
    return true;
  }
  if (env->count >= SUBST_MAX) {
    tc_error(tc, span, "too many type parameters (max %d)", SUBST_MAX);
    return false;
  }
  env->names[env->count] = name;
  env->types[env->count] = concrete;
  env->count++;
  return true;
}

static bool occurs_in(Type *unknown_ty, Type *target_ty) {
  if (target_ty == unknown_ty) {
    return true;
  }

  if (target_ty->kind == TY_UNKNOWN && target_ty->as.unknown.bound) {
    return occurs_in(unknown_ty, target_ty->as.unknown.bound);
  }

  switch (target_ty->kind) {
  case TY_TUPLE:
    for (int i = 0; i < target_ty->as.tuple.elem_count; i++)
      if (occurs_in(unknown_ty, target_ty->as.tuple.elem_types[i]))
        return true;
    return false;
  case TY_FUNCTION:
    for (int i = 0; i < target_ty->as.fun.param_count; i++)
      if (occurs_in(unknown_ty, target_ty->as.fun.param_types[i]))
        return true;
    return occurs_in(unknown_ty, target_ty->as.fun.return_type);
  case TY_STRUCT:
    for (int i = 0; i < target_ty->as.struc.type_arg_count; i++)
      if (occurs_in(unknown_ty, target_ty->as.struc.type_args[i]))
        return true;
    return false;
  case TY_ENUM:
    for (int i = 0; i < target_ty->as.enm.type_arg_count; i++)
      if (occurs_in(unknown_ty, target_ty->as.enm.type_args[i]))
        return true;
    return false;
  case TY_TRAIT:
    for (int i = 0; i < target_ty->as.trait.type_arg_count; i++)
      if (occurs_in(unknown_ty, target_ty->as.trait.type_args[i]))
        return true;
    return false;
  case TY_ARRAY:
    return occurs_in(unknown_ty, target_ty->as.array.elem_type);
  case TY_ASSOC:
    return occurs_in(unknown_ty, target_ty->as.assoc.base);
    break;
  default:
    return false;
  }
}

static bool unify(TypeChecker *tc, SubstEnv *env, Type *param_ty, Type *arg_ty,
                  Span span) {
  if (type_is_poison(param_ty) || type_is_poison(arg_ty))
    return true; // suppress

  if (types_equal(param_ty, arg_ty)) {
    return true;
  }

  if (param_ty->kind == TY_UNKNOWN) {
    if (param_ty->as.unknown.bound)
      return unify(tc, env, param_ty->as.unknown.bound, arg_ty, span);
    if (arg_ty->kind == TY_UNKNOWN && arg_ty == param_ty)
      return true;
    if (occurs_in(param_ty, arg_ty)) {
      tc_error(tc, span, "cannot unify: recursive types");
      return false;
    }
    param_ty->as.unknown.bound = arg_ty;
    return true;
  }

  if (arg_ty->kind == TY_UNKNOWN) {
    if (arg_ty->as.unknown.bound)
      return unify(tc, env, param_ty, arg_ty->as.unknown.bound, span);
    if (param_ty->kind == TY_UNKNOWN && param_ty == arg_ty)
      return true;
    if (occurs_in(arg_ty, param_ty)) {
      tc_error(tc, span, "cannot unify: recursive types");
      return false;
    }
    arg_ty->as.unknown.bound = param_ty;
    return true;
  }

  if (arg_ty->kind == TY_GENERIC || param_ty->kind == TY_GENERIC) {
    return types_equal(arg_ty, param_ty);
  }

  if (param_ty->kind != arg_ty->kind) {
    return false;
  }

  switch (param_ty->kind) {
  case TY_TUPLE:
    if (param_ty->as.tuple.elem_count != arg_ty->as.tuple.elem_count)
      return false;
    for (int i = 0; i < param_ty->as.tuple.elem_count; i++)
      if (!unify(tc, env, param_ty->as.tuple.elem_types[i],
                 arg_ty->as.tuple.elem_types[i], span))
        return false;
    return true;

  case TY_ARRAY:
    return unify(tc, env, param_ty->as.array.elem_type,
                 arg_ty->as.array.elem_type, span);

  case TY_FUNCTION:
    if (param_ty->as.fun.param_count != arg_ty->as.fun.param_count)
      return false;
    for (int i = 0; i < param_ty->as.fun.param_count; i++)
      if (!unify(tc, env, param_ty->as.fun.param_types[i],
                 arg_ty->as.fun.param_types[i], span))
        return false;
    return unify(tc, env, param_ty->as.fun.return_type,
                 arg_ty->as.fun.return_type, span);

  case TY_STRUCT:
    // same struct def required; unify type args
    if (param_ty->as.struc.def != arg_ty->as.struc.def)
      return false;
    for (int i = 0; i < param_ty->as.struc.type_arg_count; i++)
      if (!unify(tc, env, param_ty->as.struc.type_args[i],
                 arg_ty->as.struc.type_args[i], span))
        return false;
    return true;

  case TY_ENUM:
    if (param_ty->as.enm.def != arg_ty->as.enm.def)
      return false;
    for (int i = 0; i < param_ty->as.enm.type_arg_count; i++)
      if (!unify(tc, env, param_ty->as.enm.type_args[i],
                 arg_ty->as.enm.type_args[i], span))
        return false;
    return true;

  default:
    // concrete scalars (Int, Float, Bool, String, Unit) — kind equality above
    // is enough
    return true;
  }
}

static Type *substitute(TypeChecker *tc, const SubstEnv *env, Type *ty) {
  if (type_is_poison(ty)) {
    return ty;
  }

  switch (ty->kind) {
  case TY_UNKNOWN:
    if (ty->as.unknown.bound)
      return substitute(tc, env, ty->as.unknown.bound);
    return ty;
  case TY_GENERIC: {
    Type *bound = subst_lookup(env, ty->as.generic.name);
    // unbound generic after unification is a checker bug, but degrade
    // gracefully.
    return bound ? bound : ty;
  }
  case TY_TUPLE: {
    TypeArrayScratch els;
    type_array_scratch_init(&els, ty->as.tuple.elem_count, tc->al);
    bool ok = true;
    for (int i = 0; i < els.count; i++) {
      els.ptr[i] = substitute(tc, env, ty->as.tuple.elem_types[i]);
      ok = ok && !type_is_poison(els.ptr[i]);
    }
    return ok ? ty_tuple(els.ptr, els.count, tc->al) : ty_poison();
  }
    // case TY_ARRAY:
    //   return ty_array(substitute(tc, env, ty->as.array.elem_type), tc->al);
  case TY_FUNCTION: {
    TypeArrayScratch ps;
    type_array_scratch_init(&ps, ty->as.fun.param_count, tc->al);

    bool ok = true;

    for (int i = 0; i < ps.count; i++) {
      ps.ptr[i] = substitute(tc, env, ty->as.fun.param_types[i]);
      ok = ok && !type_is_poison(ps.ptr[i]);
    }

    Type *ret = substitute(tc, env, ty->as.fun.return_type);
    ok = ok && !type_is_poison(ret);
    if (!ok) {
      return ty_poison();
    }

    return ty_function(ps.ptr, ps.count, ret, tc->al);
  }
  case TY_STRUCT: {
    TypeArrayScratch args;
    type_array_scratch_init(&args, ty->as.struc.type_arg_count, tc->al);
    bool ok = true;
    for (int i = 0; i < args.count; i++) {
      args.ptr[i] = substitute(tc, env, ty->as.struc.type_args[i]);
      ok = ok && !type_is_poison(args.ptr[i]);
    }
    return ok ? ty_struct(ty->as.struc.def, args.ptr, args.count, tc->al)
              : ty_poison();
  }
  case TY_ENUM: {
    TypeArrayScratch args;
    type_array_scratch_init(&args, ty->as.enm.type_arg_count, tc->al);
    bool ok = true;
    for (int i = 0; i < args.count; i++) {
      args.ptr[i] = substitute(tc, env, ty->as.enm.type_args[i]);
      ok = ok && !type_is_poison(args.ptr[i]);
    }
    return ok ? ty_enum(ty->as.enm.def, args.ptr, args.count, tc->al)
              : ty_poison();
  }
  default:
    return ty; // concrete scalar — no substitution needed
  }
}

static PathRes resolve_path(TypeChecker *tc, Path *path) {
  USE_INTERNAL(tc, tci);

  ResCtx ctx = {.kind = RCTX_SCOPE};

  for (int i = 0; i < path->count; i++) {
    StringView seg = path->segments[i];
    bool is_last = (i == path->count - 1);

    switch (ctx.kind) {
    case RCTX_SCOPE: {
      Symbol *sym = sym_lookup(&tci->type_syms, seg);
      if (sym == NULL) {
        sym = sym_lookup(&tci->val_syms, seg);
      }

      if (sym == NULL) {
        // tc_error(tc, path->span, "unknown symbol '" SV_FMT "'", SV_ARG(seg));
        return (PathRes){.kind = PATH_RES_UNRESOLVED};
      }

      if (is_last) {
        return (PathRes){.kind = PATH_RES_SYMBOL, .as.symbol = sym};
      }

      switch (sym->kind) {
      case SYM_STRUCT:
        ctx =
            (ResCtx){.kind = RCTX_STRUCT, .as.struct_def = sym->as.struct_def};
        break;
      case SYM_ENUM:
        ctx = (ResCtx){.kind = RCTX_ENUM, .as.enum_def = sym->as.enum_def};
        break;
      default:
        tc_error(tc, path->span,
                 "member access not supported yet (symbol '" SV_FMT
                 "' is not a struct or enum)",
                 SV_ARG(seg));
        return (PathRes){.kind = PATH_RES_UNRESOLVED};
      }
      break;
    }
    case RCTX_STRUCT: {
      tc_error(tc, path->span,
               "member access not supported yet (struct context for '" SV_FMT
               "')",
               SV_ARG(seg));
      return (PathRes){.kind = PATH_RES_UNRESOLVED};
      // StructDef *def = ctx.as.struct_def;
      // MethodDef *m = struct_find_method(def, seg);
      // if (m == NULL) {
      //   return (PathRes){.kind = PATH_RES_UNRESOLVED};
      // }
      // return (PathRes){.kind = PATH_RES_METHOD,
      //                  .as.method = {.def = def, .method = m}};
    }
    case RCTX_ENUM:
      EnumDef *def = ctx.as.enum_def;
      VariantDef *v = enum_find_variant(def, seg);
      if (v == NULL) {
        return (PathRes){.kind = PATH_RES_UNRESOLVED};
      }
      return (PathRes){.kind = PATH_RES_VARIANT,
                       .as.variant = {.def = def, .variant = v}};
    }
  }

  // unreachable
  return (PathRes){.kind = PATH_RES_UNRESOLVED};
}

// ============================================================================
// resolution
// ============================================================================

static Type *resolve_typenode(TypeChecker *tc, TypeNode *tn);
static void resolve_stmt(TypeChecker *tc, Stmt *stmt);
static Type *resolve_expr(TypeChecker *tc, Expr *expr);

static Type *resolve_typenode(TypeChecker *tc, TypeNode *tn) {
  Type *result = NULL;

  switch (tn->kind) {
  case TYNODE_NAMED: {
    if (tn->as.named.path.count == 1) {
      StringView seg = tn->as.named.path.segments[0];
      if (sv_equal_cstr(seg, "Int")) {
        result = ty_int();
        break;
      } else if (sv_equal_cstr(seg, "Float")) {
        result = ty_float();
        break;
      } else if (sv_equal_cstr(seg, "Bool")) {
        result = ty_bool();
        break;
      } else if (sv_equal_cstr(seg, "String")) {
        result = ty_string();
        break;
      } else if (sv_equal_cstr(seg, "_")) {
        result = ty_unknown(NULL, tc->al);
        break;
      }
    }

    PathRes res = resolve_path(tc, &tn->as.named.path);
    if (res.kind == PATH_RES_UNRESOLVED) {
      tc_error(tc, tn->span, "unknown type '" SV_FMT "'",
               SV_ARG(tn->as.named.path.segments[0]));
      result = ty_poison();
      break;
    }

    // only symbols for now
    Symbol *sym = res.as.symbol;
    assert(sym != NULL &&
           "resolve_typenode: expected symbol resolution for named type");

    // check type argument arity
    if (sym->kind == SYM_STRUCT || sym->kind == SYM_ENUM) {
      int expected = (sym->kind == SYM_STRUCT)
                         ? sym->type->as.struc.def->type_param_count
                         : sym->type->as.enm.def->type_param_count;
      int got = tn->as.named.type_arg_count;

      if (got != expected) {
        if (expected == 0) {
          tc_error(tc, tn->span, "type '%s' does not take type arguments",
                   type_name(sym->type));
        } else if (got == 0) {
          tc_error(
              tc, tn->span,
              "type '%s' requires %d type argument(s) but none were provided",
              type_name(sym->type), expected);
        } else {
          tc_error(
              tc, tn->span,
              "type '%s' requires %d type argument(s) but %d were provided",
              type_name(sym->type), expected, got);
        }
        result = ty_poison();
        break;
      }
    }

    TypeArrayScratch args;
    bool args_ok = true;
    type_array_scratch_init(&args, tn->as.named.type_arg_count, tc->al);
    for (int i = 0; i < args.count; i++) {
      args.ptr[i] = resolve_typenode(tc, tn->as.named.type_args[i]);
      args_ok = args_ok && !type_is_poison(args.ptr[i]);
    }

    if (!args_ok) {
      result = ty_poison();
      break;
    }

    switch (sym->kind) {
    case SYM_TYPE_PARAM:
      result = sym->type;
      break;
    case SYM_STRUCT:
      if (sym->type->as.struc.def->type_param_count > 0) {
        result =
            ty_struct(sym->type->as.struc.def, args.ptr, args.count, tc->al);
        break;
      }
      result = sym->type->as.struc.def->self_type;
      break;
    case SYM_VAR:
    case SYM_FUN:
      // the name refers to a value, not a type.
      tc_error(tc, tn->span, "'" SV_FMT "' is a value, not a type",
               SV_ARG(sym->name));
      result = ty_poison();
      break;
    case SYM_ENUM:
      if (sym->type->as.enm.def->type_param_count > 0) {
        result = ty_enum(sym->type->as.enm.def, args.ptr, args.count, tc->al);
        break;
      }
      result = sym->type->as.enm.def->self_type;
      break;
    default:
      assert(false && "resolve_typenode: unhandled symbol kind in named type");
      result = ty_poison();
      break;
    }
    break;
  }

  case TYNODE_UNIT:
    result = ty_unit();
    break;

  case TYNODE_FUN: {
    TypeArrayScratch ps;
    type_array_scratch_init(&ps, tn->as.fun.param_count, tc->al);
    bool ok = true;

    for (int i = 0; i < ps.count; i++) {
      ps.ptr[i] = resolve_typenode(tc, tn->as.fun.param_types[i]);
      ok = ok && !type_is_poison(ps.ptr[i]);
    }

    Type *ret_type = resolve_typenode(tc, tn->as.fun.return_type);
    ok = ok && !type_is_poison(ret_type);

    result = ok ? ty_function(ps.ptr, ps.count, ret_type, tc->al) //
                : ty_poison();
    break;
  }

  case TYNODE_TUPLE: {
    TypeArrayScratch elms;
    type_array_scratch_init(&elms, tn->as.tuple.count, tc->al);
    bool ok = true;
    for (int i = 0; i < elms.count; i++) {
      elms.ptr[i] = resolve_typenode(tc, tn->as.tuple.elems[i]);
      ok = ok && !type_is_poison(elms.ptr[i]);
    }
    result = ok ? ty_tuple(elms.ptr, elms.count, tc->al) : ty_poison();
    break;
  }

  default:
    result = ty_poison();
    assert(false && "resolve_typenode: unhandled type node kind");
    break;
  }

  assert(result != NULL && "resolve_typenode: result is NULL");
  return result;
}

static void resolve_stmt(TypeChecker *tc, Stmt *stmt) {
  USE_INTERNAL(tc, tci);

  switch (stmt->kind) {
  case STMT_EXPR:
    switch (stmt->as.expr_stmt.expr->kind) {
    case EXPR_MATCH:
      stmt->as.expr_stmt.expr->as.match.enforce_exhaustiveness = false;
    default:
      break;
    }
    resolve_expr(tc, stmt->as.expr_stmt.expr);
    break;

  case STMT_VAR: {
    Type *init_type = resolve_expr(tc, stmt->as.var_stmt.initializer);
    Type *ann_type =
        stmt->as.var_stmt.type_annotation
            ? resolve_typenode(tc, stmt->as.var_stmt.type_annotation)
            : ty_unknown(NULL, tc->al);

    Type *resolved = init_type;
    if (!type_is_poison(ann_type) && !type_is_poison(init_type)) {
      SubstEnv env = {0};
      if (!unify(tc, &env, ann_type, init_type, stmt->span)) {
        char ann_buf[64], init_buf[64];
        type_sprintf(ann_type, ann_buf, sizeof(ann_buf));
        type_sprintf(init_type, init_buf, sizeof(init_buf));
        tc_error(tc, stmt->span,
                 "type annotation '%s' doesn't match initializer type '%s'",
                 ann_buf, init_buf);
        resolved = ty_poison();
      } else {
        resolved = substitute(tc, &env, ann_type);
      }
    }

    switch (stmt->as.var_stmt.binding.kind) {
    case BIND_IDENT: {
      Symbol sym = {
          .name = stmt->as.var_stmt.binding.as.ident,
          .kind = SYM_VAR,
          .type = resolved,
          // .slot = next_local_slot(tci->current_fun),
          // todo: assign local variable slots
      };
      sym_define(&tci->val_syms, sym, tc->al);
      break;
    }
    case BIND_TUPLE: {
      bool is_tuple = resolved->kind == TY_TUPLE;
      bool is_struct_tuple = resolved->kind == TY_STRUCT &&
                             resolved->as.struc.def->is_tuple_struct;

      bool bad_type = !is_tuple && !is_struct_tuple;

      int expected_arity = is_tuple ? resolved->as.tuple.elem_count
                           : is_struct_tuple
                               ? resolved->as.struc.def->field_count
                               : -1;

      bool bad_arity = !bad_type && (stmt->as.var_stmt.binding.as.tuple.count !=
                                     expected_arity);

      if (!type_is_poison(resolved) && bad_type) {
        char resolved_buf[64];
        type_sprintf(resolved, resolved_buf, sizeof(resolved_buf));
        tc_error(tc, stmt->span,
                 "type of initializer is '%s' but tuple binding requires a "
                 "tuple type",
                 resolved_buf);
      }

      if (!type_is_poison(resolved) && bad_arity) {
        char resolved_buf[64];
        type_sprintf(resolved, resolved_buf, sizeof(resolved_buf));
        tc_error(tc, stmt->span,
                 "tuple binding has %d elements but initializer tuple has %d "
                 "elements (type '%s')",
                 stmt->as.var_stmt.binding.as.tuple.count, expected_arity,
                 resolved_buf);
        break;
      }

      for (int i = 0; i < stmt->as.var_stmt.binding.as.tuple.count; i++) {
        Type *ty = is_tuple          ? resolved->as.tuple.elem_types[i]
                   : is_struct_tuple ? resolved->as.struc.def->fields[i].type
                                     : ty_poison();
        Symbol sym = {
            .name = stmt->as.var_stmt.binding.as.tuple.names[i],
            .kind = SYM_VAR,
            .type = ty,
            // .slot = next_local_slot(tci->current_fun),
        };
        sym_define(&tci->val_syms, sym, tc->al);
      }
      break;
    }
    case BIND_STRUCT: {
      StringView segs[] = {stmt->as.var_stmt.binding.as.struc.struct_name};
      Path path = {.segments = segs, .count = 1, .span = stmt->span};
      PathRes res = resolve_path(tc, &path);

      if (res.kind != PATH_RES_SYMBOL || res.as.symbol->kind != SYM_STRUCT) {
        tc_error(tc, stmt->span, "unknown struct '" SV_FMT "'",
                 SV_ARG(segs[0]));
        break;
      }

      if (resolved->kind != TY_STRUCT) {
        char resolved_buf[64];
        type_sprintf(resolved, resolved_buf, sizeof(resolved_buf));
        tc_error(tc, stmt->span,
                 "type of initializer is '%s' but struct binding requires a "
                 "struct type",
                 resolved_buf);
        break;
      }

      StructDef *def = res.as.symbol->as.struct_def;
      bool is_same_struct = (resolved->as.struc.def == def);

      if (!is_same_struct) {
        char resolved_buf[64];
        type_sprintf(resolved, resolved_buf, sizeof(resolved_buf));
        tc_error(tc, stmt->span,
                 "type of initializer is '%s' but struct binding requires "
                 "struct '" SV_FMT "'",
                 resolved_buf, SV_ARG(segs[0]));
      }

      for (int i = 0; i < stmt->as.var_stmt.binding.as.struc.field_count; i++) {
        FieldDef *field =
            is_same_struct
                ? struct_find_field(
                      def, stmt->as.var_stmt.binding.as.struc.field_names[i])
                : NULL;
        if (is_same_struct && field == NULL) {
          tc_error(tc, stmt->span,
                   "struct '" SV_FMT "' has no field named '" SV_FMT "'",
                   SV_ARG(segs[0]),
                   SV_ARG(stmt->as.var_stmt.binding.as.struc.field_names[i]));
        }
        Type *ty =
            field ? resolved->as.struc.def->fields[field - def->fields].type
                  : ty_poison();
        Symbol sym = {
            .name = stmt->as.var_stmt.binding.as.struc.field_names[i],
            .kind = SYM_VAR,
            .type = ty,
            // .slot = next_local_slot(tci->current_fun),
        };
        sym_define(&tci->val_syms, sym, tc->al);
      }
      break;
    }
    default:
      assert(false && "resolve_stmt: unhandled var binding kind");
      break;
    }
    break;
  }
  case STMT_RETURN: {
    Type *value_type = NULL;
    if (stmt->as.return_stmt.value != NULL) {
      value_type = resolve_expr(tc, stmt->as.return_stmt.value);
    } else {
      value_type = ty_unit();
    }

    if (tci->current_fun == NULL) {
      tc_error(tc, stmt->span, "return statement outside of function");
      break;
    }

    Type *expected = tci->current_fun->return_type;
    if (!type_is_poison(value_type) && !type_is_poison(expected)) {
      if (!types_equal(value_type, expected)) {
        char expected_buf[64], value_buf[64];
        type_sprintf(expected, expected_buf, sizeof(expected_buf));
        type_sprintf(value_type, value_buf, sizeof(value_buf));
        tc_error(tc, stmt->span,
                 "return type mismatch: expected '%s', got '%s'", expected_buf,
                 value_buf);
      }
    }
    break;
  }
  case STMT_CONTINUE:
    if (tci->loop_depth == 0) {
      tc_error(tc, stmt->span, "continue statement not within a loop");
    }
    break;
  case STMT_BREAK:
    if (tci->loop_depth == 0) {
      tc_error(tc, stmt->span, "break statement not within a loop");
    }
    break;
  default:
    assert(false && "resolve_stmt: unhandled stmt kind");
    break;
  }
}

// ============================================================================
// resolve expr helpers
// ============================================================================

static bool resolve_type_args(TypeChecker *tc, SubstEnv *env, StructDef *def,
                              Symbol *sym, TypeNode **type_args,
                              int type_arg_count, Span span) {
  if (type_arg_count == 0) {
    for (int i = 0; i < def->type_param_count; i++) {
      subst_bind(tc, env, def->type_params[i], ty_unknown(NULL, tc->al), span);
    }
    return true;
  }

  if (type_arg_count != def->type_param_count) {
    tc_error(tc, span,
             "struct '%s' has %d type parameter(s) but %d were provided",
             type_name(sym->type), def->type_param_count, type_arg_count);
    return false;
  }
  for (int i = 0; i < type_arg_count; i++) {
    Type *concrete = resolve_typenode(tc, type_args[i]);
    subst_bind(tc, env, def->type_params[i], concrete, span);
  }
  return true;
}

static bool check_field_type(TypeChecker *tc, SubstEnv *env, Type *field_type,
                             Type *init_type, StringView field_name,
                             int field_idx, bool named, Span span) {
  if (type_is_poison(init_type) || type_is_poison(field_type)) {
    return false;
  }

  if (unify(tc, env, field_type, init_type, span)) {
    return true;
  }

  Type *concrete = substitute(tc, env, field_type);
  if (!types_equal(concrete, init_type)) {
    char concrete_buf[64], init_buf[64];
    type_sprintf(concrete, concrete_buf, sizeof(concrete_buf));
    type_sprintf(init_type, init_buf, sizeof(init_buf));
    if (named) {
      tc_error(tc, span, "field '" SV_FMT "': expected type '%s', got '%s'",
               SV_ARG(field_name), concrete_buf, init_buf);
    } else {
      tc_error(tc, span, "field %d: expected type '%s', got '%s'",
               field_idx + 1, concrete_buf, init_buf);
    }
  }
  return false;
}

static bool resolve_call_type_args(TypeChecker *tc, SubstEnv *env,
                                   Expr *callee_expr, Type *callee_type,
                                   Span span) {
  PathRes res = resolve_path(tc, &callee_expr->as.path_expr.path);
  if (res.kind != PATH_RES_SYMBOL || res.as.symbol->kind != SYM_FUN)
    return true;

  FunDef *def = res.as.symbol->as.fun_def;
  int explicit_count = callee_expr->as.path_expr.type_arg_count;

  if (explicit_count == 0) {
    // if no explicit type arguments, bind all to unknowns
    for (int i = 0; i < def->type_param_count; i++) {
      subst_bind(tc, env, def->type_params[i], ty_unknown(NULL, tc->al), span);
    }
    return true;
  }

  if (explicit_count != def->type_param_count) {
    tc_error(tc, span,
             "function '%s' has %d type parameter(s) but %d were provided",
             type_name(callee_type), def->type_param_count, explicit_count);
    return false;
  }
  for (int i = 0; i < explicit_count; i++) {
    Type *concrete =
        resolve_typenode(tc, callee_expr->as.path_expr.type_args[i]);
    subst_bind(tc, env, def->type_params[i], concrete, span);
  }
  return true;
}

static Type *rewrite_tuple_struct_call(TypeChecker *tc, Expr *expr,
                                       Symbol *sym) {
  assert(expr->kind == EXPR_CALL &&
         "rewrite_tuple_struct_call: expected call expression");
  FieldInit *fields =
      al_alloc(tc->al, expr->as.call.arg_count * sizeof(FieldInit));
  for (int i = 0; i < expr->as.call.arg_count; i++) {
    fields[i] = (FieldInit){
        .name = {0},
        .value = expr->as.call.args[i],
        .span = expr->as.call.args[i]->span,
    };
  }

  TypeNode **type_args = NULL;
  int type_arg_count = 0;
  if (expr->as.call.callee->kind == EXPR_PATH) {
    type_args = expr->as.call.callee->as.path_expr.type_args;
    type_arg_count = expr->as.call.callee->as.path_expr.type_arg_count;
  }

  *expr = (Expr){
      .kind = EXPR_STRUCT_INIT,
      .span = expr->span,
      .as.struct_init =
          {
              .type_arg_count = type_arg_count,
              .type_args = type_args,
              .path = (Path){.segments = &sym->name, .count = 1},
              .field_count = expr->as.call.arg_count,
              .fields = fields,
          },
  };
  return resolve_expr(tc, expr);
}

static Type *resolve_callee(TypeChecker *tc, Expr *expr, FunDef **out_fun_def) {
  assert(expr->kind == EXPR_CALL && "resolve_callee: expected call expression");
  USE_INTERNAL(tc, tci);

  if (expr->as.call.callee->kind != EXPR_VAR)
    return resolve_expr(tc, expr->as.call.callee);

  Symbol *sym = sym_lookup(&tci->type_syms, expr->as.call.callee->as.var.name);
  if (!sym) {
    return resolve_expr(tc, expr->as.call.callee);
  }

  switch (sym->kind) {
  case SYM_FUN:
    if (out_fun_def) {
      *out_fun_def = sym->as.fun_def;
    }
    return sym->type;

  case SYM_STRUCT:
    if (!sym->type->as.struc.def->is_tuple_struct) {
      tc_error(tc, expr->span,
               "only tuple structs can be called like functions (struct "
               "'" SV_FMT "' is not a tuple struct)",
               SV_ARG(sym->name));
      return ty_poison();
    }
    return rewrite_tuple_struct_call(tc, expr, sym);

  default:
    tc_error(tc, expr->as.call.callee->span, "cannot call symbol '" SV_FMT "'",
             SV_ARG(sym->name));
    return ty_poison();
  }
}

// same as function definitions but for closures
static Type *resolve_fun_expr(TypeChecker *tc, Expr *expr) {
  assert(expr->kind == EXPR_CLOSURE &&
         "resolve_fun_expr: expected closure expression");

  FunDef *def = al_alloc_zero_for(tc->al, FunDef);

  def->type_param_count = expr->as.closure.type_param_count;
  def->type_params =
      al_alloc_zero(tc->al, def->type_param_count * sizeof(StringView));

  for (int i = 0; i < def->type_param_count; i++) {
    def->type_params[i] = expr->as.closure.type_params[i].name;
  }

  def->param_count = expr->as.closure.param_count;
  def->params = al_alloc_zero(tc->al, def->param_count * sizeof(ParamDef));

  for (int i = 0; i < def->param_count; i++) {
    def->params[i].name = expr->as.closure.params[i].name;
    def->params[i].is_self = expr->as.closure.params[i].is_self;
  }

  TypeArrayScratch ps;
  type_array_scratch_init(&ps, def->param_count, tc->al);

  for (int i = 0; i < def->param_count; i++) {
    ps.ptr[i] = ty_poison();
  }

  // todo: slot assignment

  expr->as.closure.def = def;

  // check

  USE_INTERNAL(tc, tci);

  // register type parameters as type symbols
  for (int i = 0; i < def->type_param_count; i++) {
    StringView name = expr->as.closure.type_params[i].name;
    Symbol tp_sym = {
        .name = name,
        .kind = SYM_TYPE_PARAM,
        .type = ty_generic(name, NULL, 0, tc->al),
    };
    if (expr->as.closure.type_params[i].bound_count > 0) {
      tc_error(tc, expr->as.closure.type_params[i].span,
               "type parameter bounds not supported yet");
    }
    sym_define(&tci->type_syms, tp_sym, tc->al);
  }

  // resolve parameters
  for (int i = 0; i < def->param_count; i++) {
    ClosureParam *p = &expr->as.closure.params[i];
    if (p->is_self) {
      ps.ptr[i] = ty_poison();
      def->params[i].is_self = true;
      assert(false && "resolve_fun_expr: self params not supported yet");
    } else {
      ps.ptr[i] = p->type_annotation ? resolve_typenode(tc, p->type_annotation)
                                     : ty_unknown(NULL, tc->al);
    }
  }

  // resolve return type
  Type *return_type =
      expr->as.closure.return_type_annotation
          ? resolve_typenode(tc, expr->as.closure.return_type_annotation)
          : ty_unknown(NULL, tc->al);
  def->return_type = return_type;

  FunDef *saved_fun = tci->current_fun;
  tci->current_fun = def;
  tci->loop_depth = 0;

  sym_push_scope(&tci->val_syms, tc->al);

  for (int i = 0; i < def->param_count; i++) {
    Symbol param_sym = {
        .name = def->params[i].name,
        .kind = SYM_VAR,
        .type = ps.ptr[i],
        .slot = i,
    };
    sym_define(&tci->val_syms, param_sym, tc->al);
  }

  if (expr->as.closure.body != NULL) {
    Type *body_type = resolve_expr(tc, expr->as.closure.body);

    SubstEnv env;
    subst_init(&env);

    if (!type_is_poison(body_type) && !type_is_poison(return_type)) {
      if (!unify(tc, &env, return_type, body_type,
                 expr->as.closure.body->span)) {
        char expected_buf[64], value_buf[64];
        type_sprintf(return_type, expected_buf, sizeof(expected_buf));
        type_sprintf(body_type, value_buf, sizeof(value_buf));
        tc_error(tc, expr->span,
                 "function '" SV_FMT "' returns '%s' but body has type '%s'",
                 SV_ARG(def->name), expected_buf, value_buf);
      }
    }
  }

  sym_pop_scope(&tci->val_syms, tc->al);

  tci->current_fun = saved_fun;

  return ty_function(ps.ptr, ps.count, return_type, tc->al);
}

static void check_match_pattern(TypeChecker *tc, SubstEnv *env, Pattern *pat,
                                Type *subject_ty) {
  USE_INTERNAL(tc, tci);
  pat->resolved_type = subject_ty;

  switch (pat->kind) {
  case PAT_WILDCARD:
    break;
  case PAT_LITERAL: {
    Type *literal_ty = resolve_expr(tc, pat->as.literal_expr);
    if (!type_is_poison(literal_ty) && !type_is_poison(subject_ty)) {
      if (!types_equal(literal_ty, subject_ty)) {
        char literal_buf[64], subject_buf[64];
        type_sprintf(literal_ty, literal_buf, sizeof(literal_buf));
        type_sprintf(subject_ty, subject_buf, sizeof(subject_buf));
        tc_error(tc, pat->span,
                 "pattern type '%s' does not match subject type '%s'",
                 literal_buf, subject_buf);
      }
    }
    break;
  }
  case PAT_BIND: {
    Symbol sym = {
        .name = pat->as.bind.name,
        .kind = SYM_VAR,
        .type = subject_ty,
    };
    sym_define(&tci->val_syms, sym, tc->al);
    break;
  }
  case PAT_VARIANT: {
    if (type_is_poison(subject_ty)) {
      break;
    }

    if (subject_ty->kind != TY_ENUM) {
      char subject_buf[64];
      type_sprintf(subject_ty, subject_buf, sizeof(subject_buf));
      tc_error(tc, pat->span,
               "variant pattern requires an enum subject, got type '%s'",
               subject_buf);
      break;
    }

    EnumDef *def = subject_ty->as.enm.def;
    StringView variant_name =
        pat->as.variant.path.segments[pat->as.variant.path.count - 1];
    VariantDef *variant = enum_find_variant(def, variant_name);
    if (variant == NULL) {
      char subject_buf[64];
      type_sprintf(subject_ty, subject_buf, sizeof(subject_buf));
      tc_error(tc, pat->span, "enum '%s' has no variant named '" SV_FMT "'",
               subject_buf, SV_ARG(variant_name));
      pat->resolved_type = ty_poison();
      break;
    }

    pat->as.variant.resolved_variant = variant;

    int expected = variant->payload_count;
    int got = pat->as.variant.payload_count;
    if (expected != got) {
      char subject_buf[64];
      type_sprintf(subject_ty, subject_buf, sizeof(subject_buf));
      tc_error(tc, pat->span,
               "variant pattern for '" SV_FMT
               "' expects %d payload(s) but %d were provided (enum '%s')",
               SV_ARG(variant_name), expected, got, subject_buf);
      break;
    }

    int check_n = expected < got ? expected : got;
    for (int i = 0; i < check_n; i++) {
      Type *payload_ty = variant->payload_types[i];
      Pattern *payload_pat = pat->as.variant.payloads[i];
      check_match_pattern(tc, env, payload_pat, payload_ty);
    }
    for (int i = check_n; i < got; i++) {
      check_match_pattern(tc, env, pat->as.variant.payloads[i], ty_poison());
    }
    break;
  }
  case PAT_STRUCT: {
    if (type_is_poison(subject_ty)) {
      break;
    }

    if (subject_ty->kind != TY_STRUCT) {
      char subject_buf[64];
      type_sprintf(subject_ty, subject_buf, sizeof(subject_buf));
      tc_error(tc, pat->span,
               "struct pattern requires a struct subject, got type '%s'",
               subject_buf);
      break;
    }

    for (int i = 0; i < pat->as.struc.field_count; i++) {
      FieldPat *fp = &pat->as.struc.fields[i];

      Type *field_ty = NULL;

      FieldDef *field =
          struct_find_field(subject_ty->as.struc.def, fp->field_name);
      if (field == NULL) {
        char subject_buf[64];
        type_sprintf(subject_ty, subject_buf, sizeof(subject_buf));
        tc_error(tc, fp->span, "struct '%s' has no field named '" SV_FMT "'",
                 subject_buf, SV_ARG(fp->field_name));
        field_ty = ty_poison();
      } else {
        field_ty = substitute(tc, env, field->type);
      }

      if (fp->sub_pattern != NULL) {
        check_match_pattern(tc, env, fp->sub_pattern, field_ty);
      } else {
        Symbol sym = {
            .name = fp->field_name,
            .kind = SYM_VAR,
            .type = field_ty,
        };
        sym_define(&tci->val_syms, sym, tc->al);
      }
    }

    break;
  }
  case PAT_TUPLE: {
    if (type_is_poison(subject_ty)) {
      break;
    }

    if (subject_ty->kind != TY_TUPLE) {
      char subject_buf[64];
      type_sprintf(subject_ty, subject_buf, sizeof(subject_buf));
      tc_error(tc, pat->span,
               "tuple pattern requires a tuple subject, got type '%s'",
               subject_buf);
      break;
    }

    int expected = subject_ty->as.tuple.elem_count;
    int got = pat->as.tuple.count;

    if (expected != got) {
      char subject_buf[64];
      type_sprintf(subject_ty, subject_buf, sizeof(subject_buf));
      tc_error(tc, pat->span,
               "tuple pattern expects %d elements but %d were provided (type "
               "'%s')",
               expected, got, subject_buf);
      break;
    }

    int check_n = expected < got ? expected : got;
    for (int i = 0; i < check_n; i++) {
      Type *elem_ty = subject_ty->as.tuple.elem_types[i];
      Pattern *elem_pat = pat->as.tuple.elems[i];
      check_match_pattern(tc, env, elem_pat, elem_ty);
    }
    for (int i = check_n; i < got; i++) {
      check_match_pattern(tc, env, pat->as.tuple.elems[i], ty_poison());
    }

    break;
  }
  default:
    assert(false && "check_match_pattern: unhandled pattern kind");
    break;
  }
}

static void check_match_exhaustiveness(TypeChecker *tc, Expr *match_expr,
                                       Type *subject_ty) {
  if (type_is_poison(subject_ty) || subject_ty->kind != TY_ENUM) {
    return;
  }

  EnumDef *def = subject_ty->as.enm.def;

  bool *covered = al_alloc_zero(tc->al, def->variant_count * sizeof(bool));

  for (int i = 0; i < match_expr->as.match.arm_count; i++) {
    Pattern *pat = match_expr->as.match.arms[i].pattern;
    if (pat->kind == PAT_WILDCARD || pat->kind == PAT_BIND) {
      // wildcard and bind patterns cover all variants
      return;
    }
    if (pat->kind == PAT_VARIANT && pat->as.variant.resolved_variant != NULL) {
      int idx = pat->as.variant.resolved_variant->tag;
      if (idx >= 0 && idx < def->variant_count) {
        covered[idx] = true;
      }
    }
  }

  for (int i = 0; i < def->variant_count; i++) {
    if (!covered[i]) {
      tc_error(tc, match_expr->span,
               "match is not exhaustive: missing case for variant '" SV_FMT "'",
               SV_ARG(def->variants[i].name));
    }
  }
}

static Type *resolve_expr(TypeChecker *tc, Expr *expr) {
  USE_INTERNAL(tc, tci);
  Type *result = NULL;

  switch (expr->kind) {
  case EXPR_INT:
    result = ty_int();
    break;
  case EXPR_FLOAT:
    result = ty_float();
    break;
  case EXPR_BOOL:
    result = ty_bool();
    break;
  case EXPR_STRING:
    result = ty_string();
    break;
  case EXPR_UNIT:
    result = ty_unit();
    break;

  case EXPR_VAR: {
    Symbol *sym = sym_lookup(&tci->val_syms, expr->as.var.name);
    if (sym == NULL) {
      tc_error(tc, expr->span, "undefined variable '" SV_FMT "'",
               SV_ARG(expr->as.var.name));
      result = ty_poison();
    } else {
      expr->as.var.resolved_slot = sym->slot;
      result = sym->type;
    }
    break;
  }

  case EXPR_UNARY: {
    Type *operand = resolve_expr(tc, expr->as.unary.operand);
    if (type_is_poison(operand)) {
      result = ty_poison();
      break;
    }

    TokenType op = expr->as.unary.op;
    if (op == TOKEN_MINUS) {
      if (!type_is_numeric(operand)) {
        tc_error(tc, expr->span,
                 "unary '-' operator requires a numeric operand, got '%s'",
                 type_name(operand));
        result = ty_poison();
      } else {
        result = operand;
      }
    } else if (op == TOKEN_NOT) {
      if (operand->kind != TY_BOOL) {
        tc_error(tc, expr->span,
                 "unary '!' operator requires a Bool operand, got '%s'",
                 type_name(operand));
        result = ty_poison();
      } else {
        result = ty_bool();
      }
    } else {
      assert(false && "resolve_expr: unhandled unary operator");
      result = ty_poison();
    }
    break;
  }

  case EXPR_BINARY: {
    Type *lhs = resolve_expr(tc, expr->as.binary.left);
    Type *rhs = resolve_expr(tc, expr->as.binary.right);

    if (type_is_poison(lhs) || type_is_poison(rhs)) {
      result = ty_poison();
      break;
    }

    if (lhs->kind == TY_UNKNOWN || rhs->kind == TY_UNKNOWN) {
      result = ty_unknown(NULL, tc->al);
      break;
    }

    TokenType op = expr->as.binary.op;

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
        tc_error(tc, expr->span,
                 "operator requires numeric types, got '%s' and '%s'", lhs_buf,
                 rhs_buf);
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
        tc_error(tc, expr->span,
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
        tc_error(tc, expr->span,
                 "cannot compare '%s' and '%s' with == / !=", lhs_buf, rhs_buf);
        result = ty_poison();
      } else {
        result = ty_bool();
      }
    } else if (is_logic) {
      if (lhs->kind != TY_BOOL || rhs->kind != TY_BOOL) {
        tc_error(
            tc, expr->span, "'%s' requires Bool operands, got '%s' and '%s'",
            op == TOKEN_AND ? "and" : "or", type_name(lhs), type_name(rhs));
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
        tc_error(tc, expr->span, "unsupported operator for types '%s' and '%s'",
                 lhs_buf, rhs_buf);
        result = ty_poison();
      }
    }
    break;
  }

  case EXPR_BLOCK: {
    sym_push_scope(&tci->val_syms, tc->al);

    for (int i = 0; i < expr->as.block.stmt_count; i++) {
      resolve_stmt(tc, expr->as.block.stmts[i]);
    }

    if (expr->as.block.tail_expr != NULL) {
      result = resolve_expr(tc, expr->as.block.tail_expr);
    } else {
      result = ty_unit();
    }

    sym_pop_scope(&tci->val_syms, tc->al);
    break;
  }

  case EXPR_IF: {
    Type *cond = resolve_expr(tc, expr->as.if_expr.condition);
    if (!type_is_poison(cond) && cond->kind != TY_BOOL) {
      tc_error(tc, expr->as.if_expr.condition->span,
               "if condition must be Bool, got '%s'", type_name(cond));
    }
    Type *then_t = resolve_expr(tc, expr->as.if_expr.then_block);
    if (expr->as.if_expr.else_branch != NULL) {
      Type *else_t = resolve_expr(tc, expr->as.if_expr.else_branch);
      if (!type_is_poison(then_t) && !type_is_poison(else_t)) {
        if (!types_equal(then_t, else_t)) {
          char then_buf[64], else_buf[64];
          type_sprintf(then_t, then_buf, sizeof(then_buf));
          type_sprintf(else_t, else_buf, sizeof(else_buf));
          tc_error(tc, expr->span,
                   "if/else arms have different types: '%s' vs '%s'", then_buf,
                   else_buf);
          result = ty_poison();
        } else {
          result = then_t;
        }
      } else {
        result = type_is_poison(then_t) ? else_t : then_t;
      }
    } else {
      // no else branch means the result type is unit.
      result = ty_unit();
    }
    break;
  }

  case EXPR_CALL: {
    if (expr->as.call.callee->kind == EXPR_PATH) {
      PathRes res = resolve_path(tc, &expr->as.call.callee->as.path_expr.path);
      if (res.kind == PATH_RES_VARIANT) {
        VariantDef *variant = res.as.variant.variant;
        EnumDef *enm = res.as.variant.def;

        if (variant->payload_count == 0) {
          tc_error(tc, expr->span,
                   "variant '" SV_FMT "' has no payload and cannot be called",
                   SV_ARG(variant->name));
          return ty_poison();
        }

        *expr = (Expr){
            .kind = EXPR_VARIANT,
            .span = expr->span,
            .as.variant =
                {
                    .resolved_variant = variant,
                    .resolved_enum = enm,
                    .path = expr->as.call.callee->as.path_expr.path,
                    .payloads = expr->as.call.args,
                    .payload_count = expr->as.call.arg_count,
                    .caller = expr->as.call.callee,
                },
        };
        return resolve_expr(tc, expr);
      } else if (res.kind == PATH_RES_SYMBOL) {
        Symbol *sym = res.as.symbol;
        if (sym->kind == SYM_ENUM) {
          tc_error(tc, expr->span,
                   "cannot call enum '" SV_FMT "' directly; did you mean to "
                   "construct a variant?",
                   SV_ARG(sym->name));
          return ty_poison();
        }

        if (res.as.symbol->kind == SYM_STRUCT &&
            res.as.symbol->type->as.struc.def->is_tuple_struct) {
          return rewrite_tuple_struct_call(tc, expr, res.as.symbol);
        }
      }
    }

    FunDef *fun_def = NULL;
    Type *callee_type = resolve_callee(tc, expr, &fun_def);
    if (type_is_poison(callee_type)) {
      return ty_poison();
    }

    // resolve_callee may have rewritten + re-dispatched (tuple struct path);
    // if the kind changed we already have the final result.
    if (expr->kind != EXPR_CALL) {
      return callee_type;
    }

    if (callee_type->kind != TY_FUNCTION) {
      tc_error(tc, expr->span, "called value is not a function (got '%s')",
               type_name(callee_type));
      return ty_poison();
    }

    int expected = callee_type->as.fun.param_count;
    int got = expr->as.call.arg_count;
    if (expected != got) {
      tc_error(tc, expr->span,
               "function expects %d argument(s) but %d were provided", expected,
               got);
      return ty_poison();
    }

    SubstEnv env;
    subst_init(&env);

    if (expr->as.call.callee->kind == EXPR_PATH) {
      if (!resolve_call_type_args(tc, &env, expr->as.call.callee, callee_type,
                                  expr->as.call.callee->span))
        return ty_poison();
    } else if (fun_def) {
      for (int i = 0; i < fun_def->type_param_count; i++) {
        subst_bind(tc, &env, fun_def->type_params[i], ty_unknown(NULL, tc->al),
                   expr->span);
      }
    }

    bool ok = true;
    for (int i = 0; i < got; i++) {
      Type *arg_ty = resolve_expr(tc, expr->as.call.args[i]);
      Type *param_ty = substitute(tc, &env, callee_type->as.fun.param_types[i]);
      if (type_is_poison(arg_ty) || type_is_poison(param_ty)) {
        ok = false;
        continue;
      }
      if (!unify(tc, &env, param_ty, arg_ty, expr->as.call.args[i]->span)) {
        Type *concrete = substitute(tc, &env, param_ty);
        char concrete_buf[64], arg_buf[64];
        type_sprintf(concrete, concrete_buf, sizeof(concrete_buf));
        type_sprintf(arg_ty, arg_buf, sizeof(arg_buf));
        if (!types_equal(concrete, arg_ty))
          tc_error(tc, expr->as.call.args[i]->span,
                   "argument %d: expected '%s', got '%s'", i + 1, concrete_buf,
                   arg_buf);
        ok = false;
      }
    }
    result = ok ? substitute(tc, &env, callee_type->as.fun.return_type) //
                : ty_poison();
    break;
  }

  case EXPR_ASSIGN: {
    Type *target_type = resolve_expr(tc, expr->as.assign.target);
    Type *val_type = resolve_expr(tc, expr->as.assign.value);
    if (!type_is_poison(target_type) && !type_is_poison(val_type)) {
      if (!types_equal(target_type, val_type)) {
        tc_error(tc, expr->span,
                 "cannot assign '%s' to a variable of type '%s'",
                 type_name(val_type), type_name(target_type));
        result = ty_poison();
      } else {
        result = target_type;
      }
    } else {
      result = ty_poison();
    }
    break;
  }

  case EXPR_STRUCT_INIT: {
    PathRes res = resolve_path(tc, &expr->as.struct_init.path);
    if (res.kind == PATH_RES_UNRESOLVED) {
      tc_error(tc, expr->span, "unknown struct '" SV_FMT "'",
               SV_ARG(expr->as.struct_init.path.segments[0]));
      return ty_poison();
    }

    Symbol *sym = res.as.symbol;
    if (!sym)
      return ty_poison();
    if (sym->kind != SYM_STRUCT) {
      tc_error(tc, expr->span, "'" SV_FMT "' is not a struct",
               SV_ARG(sym->name));
      return ty_poison();
    }

    StructDef *def = sym->as.struct_def;
    bool is_tuple = def->is_tuple_struct;
    int provided = expr->as.struct_init.field_count;
    bool ok = true;

    // field-count check
    if (provided != def->field_count) {
      tc_error(tc, expr->span,
               "struct '" SV_FMT "' expects %d fields but %d were provided",
               SV_ARG(def->name), def->field_count, provided);
      return ty_poison();
    }

    // named vs positional style check
    if (provided > 0) {
      bool has_name = expr->as.struct_init.fields[0].name.chars != NULL;
      if (is_tuple && has_name) {
        tc_error(tc, expr->as.struct_init.fields[0].span,
                 "tuple struct '" SV_FMT
                 "' cannot be initialized with named fields",
                 SV_ARG(def->name));
        return ty_poison();
      }
      if (!is_tuple && !has_name) {
        tc_error(tc, expr->as.struct_init.fields[0].span,
                 "struct '" SV_FMT "' must be initialized with named fields",
                 SV_ARG(def->name));
        return ty_poison();
      }
    }

    SubstEnv env;
    subst_init(&env);
    if (!resolve_type_args(tc, &env, def, sym, expr->as.struct_init.type_args,
                           expr->as.struct_init.type_arg_count, expr->span)) {
      return ty_poison();
    }

    for (int i = 0; i < provided; i++) {
      FieldInit *fi = &expr->as.struct_init.fields[i];
      Type *init_t = resolve_expr(tc, fi->value);
      Type *field_t;
      StringView fname;

      if (is_tuple) {
        field_t = substitute(tc, &env, def->fields[i].type);
        fname = (StringView){0};
      } else {
        FieldDef *fdef = struct_find_field(def, fi->name);
        if (!fdef) {
          tc_error(tc, fi->span,
                   "struct '" SV_FMT "' has no field named '" SV_FMT "'",
                   SV_ARG(def->name), SV_ARG(fi->name));
          ok = false;
          continue;
        }
        field_t = substitute(tc, &env, fdef->type);
        fname = fdef->name;
      }

      if (!check_field_type(tc, &env, field_t, init_t, fname, i, !is_tuple,
                            fi->span))
        ok = false;
    }

    result = ok ? substitute(tc, &env, sym->type) : ty_poison();
    break;
  }

  case EXPR_FIELD: {
    Type *target_type = resolve_expr(tc, expr->as.field.object);
    if (type_is_poison(target_type)) {
      result = ty_poison();
      break;
    }

    if (expr->as.field.is_tuple_field) {
      switch (target_type->kind) {
      case TY_TUPLE: {
        int idx = expr->as.field.tuple_index;
        if (idx < 0 || idx >= target_type->as.tuple.elem_count) {
          tc_error(tc, expr->span,
                   "tuple has no field at index %d (tuple has %d fields)", idx,
                   target_type->as.tuple.elem_count);
          result = ty_poison();
        } else {
          result = target_type->as.tuple.elem_types[idx];
        }
        break;
      }
      case TY_STRUCT: {
        StructDef *def = target_type->as.struc.def;
        if (!def->is_tuple_struct) {
          tc_error(tc, expr->span,
                   "struct '" SV_FMT "' is not a tuple struct, cannot access "
                   "tuple fields with '.'",
                   SV_ARG(def->name));
          result = ty_poison();
          break;
        }
        int idx = expr->as.field.tuple_index;
        if (idx < 0 || idx >= def->field_count) {
          tc_error(tc, expr->span,
                   "struct '" SV_FMT "' has no tuple field at index %d",
                   SV_ARG(def->name), idx);
          result = ty_poison();
        } else {
          result = def->fields[idx].type;
        }
        break;
      }
      default:
        tc_error(tc, expr->span, "cannot access tuple field of type '%s'",
                 type_name(target_type));
        result = ty_poison();
      }
    } else {
      switch (target_type->kind) {
      case TY_STRUCT: {
        StructDef *def = target_type->as.struc.def;
        if (def->is_tuple_struct) {
          tc_error(tc, expr->span,
                   "struct '" SV_FMT "' is a tuple struct, cannot access "
                   "fields with names",
                   SV_ARG(def->name));
          result = ty_poison();
          break;
        }
        StringView field_name = expr->as.field.field_name;
        FieldDef *field_def = NULL;
        for (int i = 0; i < def->field_count; i++) {
          if (sv_equal(def->fields[i].name, field_name)) {
            field_def = &def->fields[i];
            break;
          }
        }
        if (field_def == NULL) {
          tc_error(tc, expr->span,
                   "struct '" SV_FMT "' has no field named '" SV_FMT "'",
                   SV_ARG(def->name), SV_ARG(field_name));
          result = ty_poison();
        } else {
          SubstEnv env;
          subst_init(&env);
          for (int i = 0; i < def->type_param_count; i++) {
            Type *concrete = target_type->as.struc.type_args[i];
            subst_bind(tc, &env, def->type_params[i], concrete,
                       expr->as.field.object->span);
          }
          if (target_type->as.struc.type_arg_count > 0) {
            result = substitute(tc, &env, field_def->type);
          } else {
            result = field_def->type;
          }
        }
        break;
      }
      default:
        result = ty_poison();
        tc_error(tc, expr->span,
                 "cannot access field '" SV_FMT "' of type '%s'",
                 SV_ARG(expr->as.field.field_name), type_name(target_type));
      }
    }
    break;
  }

  case EXPR_TUPLE: {
    TypeArrayScratch elms;
    type_array_scratch_init(&elms, expr->as.tuple.count, tc->al);
    bool ok = true;
    for (int i = 0; i < elms.count; i++) {
      elms.ptr[i] = resolve_expr(tc, expr->as.tuple.elems[i]);
      ok = ok && !type_is_poison(elms.ptr[i]);
    }
    result = ok ? ty_tuple(elms.ptr, elms.count, tc->al) : ty_poison();
    break;
  }

  case EXPR_PATH: {
    Path *path = &expr->as.path_expr.path;

    PathRes res = resolve_path(tc, path);
    if (res.kind == PATH_RES_UNRESOLVED) {
      tc_error(tc, expr->span, "unresolved path '" SV_FMT "'",
               SV_ARG(path->segments[0]));
      result = ty_poison();
      break;
    }

    switch (res.kind) {
    case PATH_RES_SYMBOL:
      Symbol *sym = res.as.symbol;
      switch (sym->kind) {
      case SYM_FUN:
        result = sym->type;
        break;
      default:
        tc_error(tc, expr->span, "unsupported symbol kind in path expression");
        result = ty_poison();
        break;
      }
      break;
    case PATH_RES_VARIANT: {
      VariantDef *v = res.as.variant.variant;
      if (v->payload_count > 0) {
        tc_error(tc, expr->span,
                 "variant '" SV_FMT "' cannot be used as a value (did you "
                 "forget to call it with %d argument(s)?",
                 SV_ARG(v->name), v->payload_count);
        result = ty_poison();
        break;
      }

      Expr *new_expr = ast_expr(EXPR_VARIANT, expr->span, tc->al);

      new_expr->as.variant.resolved_variant = v;
      new_expr->as.variant.resolved_enum = res.as.variant.def;
      new_expr->as.variant.path = expr->as.path_expr.path;
      new_expr->as.variant.caller = expr;

      return resolve_expr(tc, new_expr);
    }
    default:
      tc_error(tc, expr->span, "unsupported path resolution kind");
      result = ty_poison();
    }
    break;
  }

  case EXPR_VARIANT: {
    VariantDef *v = expr->as.variant.resolved_variant;
    EnumDef *e = expr->as.variant.resolved_enum;

    if (v->payload_count != expr->as.variant.payload_count) {
      tc_error(
          tc, expr->span,
          "variant '" SV_FMT "' expects %d argument(s) but %d were provided ",
          SV_ARG(v->name), v->payload_count, expr->as.variant.payload_count);
      result = ty_poison();
      break;
    }

    SubstEnv env;
    bool ok = true;
    subst_init(&env);

    if (expr->as.variant.caller->kind == EXPR_PATH) {
      Expr *callee_expr = expr->as.variant.caller;
      int explicit_count = callee_expr->as.path_expr.type_arg_count;

      if (explicit_count == 0) {
        for (int i = 0; i < e->type_param_count; i++) {
          subst_bind(tc, &env, e->type_params[i], ty_unknown(NULL, tc->al),
                     callee_expr->span);
        }
      } else {
        if (explicit_count != e->type_param_count) {
          tc_error(tc, callee_expr->span,
                   "enum '" SV_FMT
                   "' has %d type parameter(s) but %d were provided",
                   SV_ARG(e->name), e->type_param_count, explicit_count);
          result = ty_poison();
          break;
        }
        for (int i = 0; i < explicit_count; i++) {
          Type *concrete =
              resolve_typenode(tc, callee_expr->as.path_expr.type_args[i]);
          subst_bind(tc, &env, e->type_params[i], concrete, callee_expr->span);
        }
      }
    }

    for (int i = 0; i < v->payload_count; i++) {
      Type *arg_ty = resolve_expr(tc, expr->as.variant.payloads[i]);
      Type *param_ty = substitute(tc, &env, v->payload_types[i]);

      if (type_is_poison(arg_ty) || type_is_poison(param_ty)) {
        ok = false;
        continue;
      }

      bool unified =
          unify(tc, &env, param_ty, arg_ty, expr->as.variant.payloads[i]->span);
      if (!unified) {
        Type *concrete_param = substitute(tc, &env, param_ty);
        if (!types_equal(concrete_param, arg_ty)) {
          char concrete_buf[64], arg_buf[64];
          type_sprintf(concrete_param, concrete_buf, sizeof(concrete_buf));
          type_sprintf(arg_ty, arg_buf, sizeof(arg_buf));
          tc_error(tc, expr->as.variant.payloads[i]->span,
                   "argument %d: expected '%s', got '%s'", i + 1, concrete_buf,
                   arg_buf);
        }
        ok = false;
      }
    }

    if (!ok) {
      result = ty_poison();
      break;
    }

    TypeArrayScratch args;
    type_array_scratch_init(&args, e->type_param_count, tc->al);
    for (int i = 0; i < e->type_param_count; i++) {
      Type *bound = subst_lookup(&env, e->type_params[i]);
      args.ptr[i] = bound ? bound : ty_unknown(NULL, tc->al);
    }

    result = ty_enum(e, args.ptr, args.count, tc->al);
    break;
  }

  case EXPR_FOR: {
    tci->loop_depth++;
    sym_push_scope(&tci->type_syms, tc->al);

    if (expr->as.for_expr.iterable != NULL) {
      Type *iterable_type = resolve_expr(tc, expr->as.for_expr.iterable);
      if (!type_is_poison(iterable_type)) {
        tc_error(tc, expr->span,
                 "for loops over iterables not supported yet "
                 "(got type '%s')",
                 type_name(iterable_type));
      }
    }

    if (expr->as.for_expr.condition != NULL) {
      Type *cond_type = resolve_expr(tc, expr->as.for_expr.condition);
      if (!type_is_poison(cond_type) && cond_type->kind != TY_BOOL) {
        tc_error(tc, expr->as.for_expr.condition->span,
                 "for loop condition must be Bool, got '%s'",
                 type_name(cond_type));
      }
    }

    if (expr->as.for_expr.body != NULL) {
      resolve_expr(tc, expr->as.for_expr.body);
    }

    sym_pop_scope(&tci->type_syms, tc->al);
    tci->loop_depth--;

    result = ty_unit();
    break;
  }
  case EXPR_CLOSURE: {
    result = resolve_fun_expr(tc, expr);
    break;
  }
  case EXPR_MATCH:
    Type *subject_ty = resolve_expr(tc, expr->as.match.subject);

    Type *result_type = NULL;

    SubstEnv env;
    subst_init(&env);

    for (int i = 0; i < expr->as.match.arm_count; i++) {
      MatchArm *arm = &expr->as.match.arms[i];

      sym_push_scope(&tci->val_syms, tc->al);

      check_match_pattern(tc, &env, arm->pattern, subject_ty);

      if (arm->guard != NULL) {
        Type *guard_ty = resolve_expr(tc, arm->guard);
        if (!type_is_poison(guard_ty) && guard_ty->kind != TY_BOOL) {
          tc_error(tc, arm->guard->span, "match guard must be Bool, got '%s'",
                   type_name(guard_ty));
        }
      }

      Type *arm_ty = resolve_expr(tc, arm->body);

      sym_pop_scope(&tci->val_syms, tc->al);

      if (result_type == NULL) {
        result_type = arm_ty;
      } else if (!type_is_poison(result_type) && !type_is_poison(arm_ty)) {
        if (!unify(tc, &env, result_type, arm_ty, arm->body->span)) {
          Type *concrete_result = substitute(tc, &env, result_type);
          Type *concrete_arm = substitute(tc, &env, arm_ty);
          char result_buf[64], arm_buf[64];
          type_sprintf(concrete_result, result_buf, sizeof(result_buf));
          type_sprintf(concrete_arm, arm_buf, sizeof(arm_buf));
          tc_error(tc, arm->body->span,
                   "match arm has type '%s' which does not match previous arms "
                   "of type '%s'",
                   arm_buf, result_buf);
          result_type = ty_poison();
        }
      } else if (type_is_poison(result_type)) {
        // recover
        result_type = arm_ty;
      }
    }

    if (expr->as.match.enforce_exhaustiveness) {
      check_match_exhaustiveness(tc, expr, subject_ty);
    }

    result = result_type ? result_type : ty_unit();
    break;
  default:
    result = ty_poison();
    assert(false && "resolve_expr: unhandled expr kind");
    break;
  }

  assert(result != NULL && "check_expr: result is NULL");
  expr->resolved_type = result;
  return result;
}

// ============================================================================
// registration
// ============================================================================

static void register_fun_decl(TypeChecker *tc, Decl *decl) {
  USE_INTERNAL(tc, tci);

  FunDef *def = al_alloc_zero_for(tc->al, FunDef);
  def->name = decl->as.fun_decl.name;

  def->type_param_count = decl->as.fun_decl.type_param_count;
  def->type_params =
      al_alloc_zero(tc->al, def->type_param_count * sizeof(StringView));

  for (int i = 0; i < def->type_param_count; i++) {
    def->type_params[i] = decl->as.fun_decl.type_params[i].name;
  }

  def->param_count = decl->as.fun_decl.param_count;
  def->params = al_alloc_zero(tc->al, def->param_count * sizeof(ParamDef));

  for (int i = 0; i < def->param_count; i++) {
    def->params[i].name = decl->as.fun_decl.params[i].name;
    def->params[i].is_self = decl->as.fun_decl.params[i].is_self;
  }

  TypeArrayScratch ps;
  type_array_scratch_init(&ps, def->param_count, tc->al);

  for (int i = 0; i < def->param_count; i++) {
    ps.ptr[i] = ty_poison();
  }

  // todo: slot assignment

  decl->as.fun_decl.def = def;

  Type *ty = ty_function(ps.ptr, ps.count, ty_unit(), tc->al);
  Symbol sym = {
      .name = def->name,
      .kind = SYM_FUN,
      .type = ty,
      .slot = def->slot,
      .as.fun_def = def,
  };
  sym_define(&tci->type_syms, sym, tc->al);
}

static void register_struct_decl(TypeChecker *tc, Decl *decl) {
  USE_INTERNAL(tc, tci);

  StructDef *def = al_alloc_zero_for(tc->al, StructDef);
  def->name = decl->as.struct_decl.name;
  def->is_tuple_struct = decl->as.struct_decl.is_tuple_struct;
  def->type_param_count = decl->as.struct_decl.type_param_count;

  def->type_params =
      al_alloc_zero(tc->al, def->type_param_count * sizeof(StringView));
  for (int i = 0; i < def->type_param_count; i++) {
    def->type_params[i] = decl->as.struct_decl.type_params[i].name;
  }

  if (def->is_tuple_struct) {
    def->field_count = decl->as.struct_decl.tuple_type_count;
    def->fields = al_alloc_zero(tc->al, def->field_count * sizeof(FieldDef));
  } else {
    def->field_count = decl->as.struct_decl.field_count;
    def->fields = al_alloc_zero(tc->al, def->field_count * sizeof(FieldDef));
    for (int i = 0; i < def->field_count; i++) {
      def->fields[i].name = decl->as.struct_decl.fields[i].name;
    }
  }

  // todo: assign struct slots

  TypeArrayScratch args;
  type_array_scratch_init(&args, def->type_param_count, tc->al);
  for (int i = 0; i < def->type_param_count; i++) {
    args.ptr[i] =
        ty_generic(decl->as.struct_decl.type_params[i].name, NULL, 0, tc->al);
  }

  Type *ty = ty_struct(def, args.ptr, args.count, tc->al);

  def->self_type = ty;
  decl->as.struct_decl.def = def;

  Symbol sym = {
      .name = def->name,
      .kind = SYM_STRUCT,
      .type = ty,
      .slot = def->slot,
      .as.struct_def = def,
  };
  sym_define(&tci->type_syms, sym, tc->al);
}

static void register_enum_decl(TypeChecker *tc, Decl *decl) {
  USE_INTERNAL(tc, tci);

  EnumDef *def = al_alloc_zero_for(tc->al, EnumDef);
  def->name = decl->as.enum_decl.name;
  def->type_param_count = decl->as.enum_decl.type_param_count;

  def->type_params =
      al_alloc_zero(tc->al, def->type_param_count * sizeof(StringView));
  for (int i = 0; i < def->type_param_count; i++) {
    def->type_params[i] = decl->as.enum_decl.type_params[i].name;
  }

  int variant_count = decl->as.enum_decl.variant_count;
  def->variant_count = variant_count;

  def->variants = al_alloc_zero(tc->al, variant_count * sizeof(VariantDef));
  for (int i = 0; i < variant_count; i++) {
    VariantDef *variant = &def->variants[i];
    variant->name = decl->as.enum_decl.variants[i].name;
    variant->payload_count = decl->as.enum_decl.variants[i].payload_count;
    variant->payload_types =
        al_alloc_zero(tc->al, variant->payload_count * sizeof(Type *));
    for (int j = 0; j < variant->payload_count; j++) {
      variant->payload_types[j] = ty_poison();
    }
  }

  // todo: assign enum slots

  TypeArrayScratch args;
  type_array_scratch_init(&args, def->type_param_count, tc->al);
  for (int i = 0; i < def->type_param_count; i++) {
    args.ptr[i] =
        ty_generic(decl->as.enum_decl.type_params[i].name, NULL, 0, tc->al);
  }

  Type *ty = ty_enum(def, args.ptr, args.count, tc->al);

  def->self_type = ty;
  decl->as.enum_decl.def = def;

  Symbol sym = {
      .name = def->name,
      .kind = SYM_ENUM,
      .type = ty,
      .slot = def->slot,
      .as.enum_def = def,
  };
  sym_define(&tci->type_syms, sym, tc->al);
}

static void register_decl(TypeChecker *tc, Decl *decl) {
  switch (decl->kind) {
  case DECL_FUN:
    register_fun_decl(tc, decl);
    break;
  case DECL_STRUCT:
    register_struct_decl(tc, decl);
    break;
  case DECL_ENUM:
    register_enum_decl(tc, decl);
    break;
  default:
    assert(false && "register_decl: unhandled decl kind");
    break;
  }
}

// ============================================================================
// re-write ambiguous
// ============================================================================

// ============================================================================
// checking
// ============================================================================

static void check_fun_decl(TypeChecker *tc, Decl *decl) {
  USE_INTERNAL(tc, tci);

  FunDef *def = decl->as.fun_decl.def;

  sym_push_scope(&tci->type_syms, tc->al);

  // register type parameters as type symbols
  for (int i = 0; i < def->type_param_count; i++) {
    StringView name = decl->as.fun_decl.type_params[i].name;
    Symbol tp_sym = {
        .name = name,
        .kind = SYM_TYPE_PARAM,
        .type = ty_generic(name, NULL, 0, tc->al),
    };
    if (decl->as.fun_decl.type_params[i].bound_count > 0) {
      tc_error(tc, decl->as.fun_decl.type_params[i].span,
               "type parameter bounds not supported yet");
    }
    sym_define(&tci->type_syms, tp_sym, tc->al);
  }

  // resolve parameters
  TypeArrayScratch ps;
  type_array_scratch_init(&ps, def->param_count, tc->al);

  for (int i = 0; i < def->param_count; i++) {
    ParamDeclNode *p = &decl->as.fun_decl.params[i];
    if (p->is_self) {
      ps.ptr[i] = ty_poison();
      def->params[i].is_self = true;
      assert(false && "check_fun_decl: self params not supported yet");
    } else {
      ps.ptr[i] = resolve_typenode(tc, p->type_annotation);
    }
  }

  // resolve return type
  Type *return_type = decl->as.fun_decl.return_type
                          ? resolve_typenode(tc, decl->as.fun_decl.return_type)
                          : ty_unit();
  def->return_type = return_type;

  Type *new_ty = ty_function(ps.ptr, ps.count, return_type, tc->al);
  Symbol *sym = sym_lookup(&tci->type_syms, def->name);
  assert(sym && sym->kind == SYM_FUN &&
         "check_fun_decl: function symbol not found");
  sym->type = new_ty;

  FunDef *saved_fun = tci->current_fun;
  tci->current_fun = def;
  tci->loop_depth = 0;

  sym_push_scope(&tci->val_syms, tc->al);

  for (int i = 0; i < def->param_count; i++) {
    Symbol param_sym = {
        .name = def->params[i].name,
        .kind = SYM_VAR,
        .type = ps.ptr[i],
        .slot = i,
    };
    sym_define(&tci->val_syms, param_sym, tc->al);
  }

  if (decl->as.fun_decl.body != NULL) {
    Type *body_type = resolve_expr(tc, decl->as.fun_decl.body);

    if (!type_is_poison(body_type) && !type_is_poison(return_type)) {
      if (!types_equal(body_type, return_type)) {
        char expected_buf[64], value_buf[64];
        type_sprintf(return_type, expected_buf, sizeof(expected_buf));
        type_sprintf(body_type, value_buf, sizeof(value_buf));
        tc_error(tc, decl->span,
                 "function '" SV_FMT "' returns '%s' but body has type '%s'",
                 SV_ARG(def->name), expected_buf, value_buf);
      }
    }
  }

  sym_pop_scope(&tci->val_syms, tc->al);
  sym_pop_scope(&tci->type_syms, tc->al);

  tci->current_fun = saved_fun;
}

static void check_struct_decl(TypeChecker *tc, Decl *decl) {
  USE_INTERNAL(tc, tci);
  StructDef *def = decl->as.struct_decl.def;

  sym_push_scope(&tci->type_syms, tc->al);

  // register type parameters as type symbols
  for (int i = 0; i < def->type_param_count; i++) {
    StringView name = decl->as.struct_decl.type_params[i].name;
    Symbol tp_sym = {
        .name = name,
        .kind = SYM_TYPE_PARAM,
        .type = ty_generic(name, NULL, 0, tc->al),
    };
    if (decl->as.struct_decl.type_params[i].bound_count > 0) {
      tc_error(tc, decl->as.struct_decl.type_params[i].span,
               "type parameter bounds not supported yet");
    }
    sym_define(&tci->type_syms, tp_sym, tc->al);
  }

  if (def->is_tuple_struct) {
    for (int i = 0; i < def->field_count; i++) {
      def->fields[i].type =
          resolve_typenode(tc, decl->as.struct_decl.tuple_types[i]);
    }
  } else {
    for (int i = 0; i < def->field_count; i++) {
      def->fields[i].type =
          resolve_typenode(tc, decl->as.struct_decl.fields[i].type_annotation);
    }
  }

  sym_pop_scope(&tci->type_syms, tc->al);
}

static void check_enum_decl(TypeChecker *tc, Decl *decl) {
  USE_INTERNAL(tc, tci);
  EnumDef *def = decl->as.enum_decl.def;

  sym_push_scope(&tci->type_syms, tc->al);

  // register type parameters as type symbols
  for (int i = 0; i < def->type_param_count; i++) {
    StringView name = decl->as.enum_decl.type_params[i].name;
    Symbol tp_sym = {
        .name = name,
        .kind = SYM_TYPE_PARAM,
        .type = ty_generic(name, NULL, 0, tc->al),
    };
    if (decl->as.enum_decl.type_params[i].bound_count > 0) {
      tc_error(tc, decl->as.enum_decl.type_params[i].span,
               "type parameter bounds not supported yet");
    }
    sym_define(&tci->type_syms, tp_sym, tc->al);
  }

  for (int i = 0; i < def->variant_count; i++) {
    VariantDef *variant = &def->variants[i];
    for (int j = 0; j < variant->payload_count; j++) {
      variant->payload_types[j] =
          resolve_typenode(tc, decl->as.enum_decl.variants[i].payload_types[j]);
    }
  }

  sym_pop_scope(&tci->type_syms, tc->al);
}

static void check_decl(TypeChecker *tc, Decl *decl) {
  switch (decl->kind) {
  case DECL_FUN:
    check_fun_decl(tc, decl);
    break;
  case DECL_STRUCT:
    check_struct_decl(tc, decl);
    break;
  case DECL_ENUM:
    check_enum_decl(tc, decl);
    break;
  default:
    assert(false && "check_decl: unhandled decl kind");
    break;
  }
}

// ============================================================================
// public api
// ============================================================================

void tc_init(TypeChecker *tc, ErrorReporter *reporter, Allocator *al) {
  tc->reporter = reporter;
  tc->al = al;
  tc->internal = al_alloc_zero_for(al, TypeCheckerInternal);

  USE_INTERNAL(tc, tci);

  tci->tc = tc;
  tci->loop_depth = 0;
  tci->current_fun = NULL;
  sym_table_init(&tci->type_syms, al);
  sym_table_init(&tci->val_syms, al);
}

void tc_destroy(TypeChecker *tc) {
  USE_INTERNAL(tc, tci);

  sym_table_destroy(&tci->type_syms, tc->al);
  sym_table_destroy(&tci->val_syms, tc->al);
  al_free(tc->al, tc->internal, sizeof(TypeCheckerInternal));

  tc->internal = NULL;
}

void tc_check_program(TypeChecker *tc, Program *program) {
  for (int i = 0; i < program->decl_count; i++) {
    register_decl(tc, program->decls[i]);
  }

  for (int i = 0; i < program->decl_count; i++) {
    check_decl(tc, program->decls[i]);
  }
}