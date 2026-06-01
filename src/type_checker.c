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
  SYM_TRAIT,
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
    TraitDef *trait_def;
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
  Type *current_self_type;
} TypeCheckerInternal;

#define USE_INTERNAL(_tc, name)                                                \
  assert((_tc)->internal);                                                     \
  TypeCheckerInternal *name = (TypeCheckerInternal *)(_tc)->internal

// ============================================================================
// internals implementation
// ============================================================================

// subtract span b from a, returning the remaining parts of a before and after b
// can be different lines
static Span span_subtract(Span a, Span b) {
  if (a.line == b.line) {
    // same line: can only have a single remaining part
    if (a.col < b.col) {
      return (Span){
          .line = a.line, .col = a.col, .line_end = a.line, .col_end = b.col};
    } else if (a.col_end > b.col_end) {
      return (Span){.line = a.line,
                    .col = b.col_end,
                    .line_end = a.line,
                    .col_end = a.col_end};
    } else {
      return (Span){0}; // no remaining span
    }
  } else {
    // different lines: can have two remaining parts, but we return the first
    // one
    if (a.line < b.line) {
      return (Span){
          .line = a.line, .col = a.col, .line_end = a.line, .col_end = 1000};
    } else if (a.line_end > b.line_end) {
      return (Span){.line = a.line_end,
                    .col = 0,
                    .line_end = a.line_end,
                    .col_end = a.col_end};
    } else {
      return (Span){0}; // no remaining span
    }
  }
}

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

static MethodDef *struct_find_method(const StructDef *def, StringView name) {
  for (int i = 0; i < def->method_count; i++) {
    if (sv_equal(def->methods[i].name, name)) {
      return &def->methods[i];
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
    for (int i = 0; i < target_ty->as.trait.def->type_param_count; i++)
      if (occurs_in(unknown_ty, target_ty->as.trait.def->type_params[i]))
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

      // if (sv_equal_cstr(seg, "Self") && tci->current_self_type) {
      //   assert(tci->current_self_type->kind == TY_STRUCT &&
      //          "Self type should always be a struct");

      //   static Symbol tmp_sym;
      //   // todo:
      //   tmp_sym = (Symbol){
      //       .name = seg,
      //       .kind = SYM_STRUCT,
      //       .type = tci->current_self_type,
      //       .as.struct_def = tci->current_self_type->as.struc.def,
      //   };
      //   sym = &tmp_sym;
      // }

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
      StructDef *def = ctx.as.struct_def;
      MethodDef *m = struct_find_method(def, seg);
      if (m == NULL) {
        return (PathRes){.kind = PATH_RES_UNRESOLVED};
      }
      return (PathRes){.kind = PATH_RES_METHOD,
                       .as.method = {.def = def, .method = m}};
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
static Type *resolve_expr(TypeChecker *tc, Expr *expr, Type *hint);

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

  case TYNODE_SELF: {
    USE_INTERNAL(tc, tci);
    if (tci->current_self_type == NULL) {
      tc_error(tc, tn->span, "'Self' is not valid in this context");
      result = ty_poison();
    } else {
      result = tci->current_self_type;
    }
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
    resolve_expr(tc, stmt->as.expr_stmt.expr, NULL);
    break;

  case STMT_VAR: {
    Type *ann_type =
        stmt->as.var_stmt.type_annotation
            ? resolve_typenode(tc, stmt->as.var_stmt.type_annotation)
            : ty_unknown(NULL, tc->al);
    Type *init_type = resolve_expr(tc, stmt->as.var_stmt.initializer, ann_type);

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
      value_type = resolve_expr(tc, stmt->as.return_stmt.value, NULL);
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
      assert(def->type_params[i]->kind == TY_GENERIC);
      subst_bind(tc, env, def->type_params[i]->as.generic.name,
                 ty_unknown(NULL, tc->al), span);
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
    assert(def->type_params[i]->kind == TY_GENERIC);
    Type *concrete = resolve_typenode(tc, type_args[i]);
    subst_bind(tc, env, def->type_params[i]->as.generic.name, concrete, span);
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
      assert(def->type_params[i]->kind == TY_GENERIC);
      subst_bind(tc, env, def->type_params[i]->as.generic.name,
                 ty_unknown(NULL, tc->al), span);
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
    subst_bind(tc, env, def->type_params[i]->as.generic.name, concrete, span);
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
  return resolve_expr(tc, expr, NULL);
}

static Type *resolve_callee(TypeChecker *tc, Expr *expr, FunDef **out_fun_def) {
  assert(expr->kind == EXPR_CALL && "resolve_callee: expected call expression");
  USE_INTERNAL(tc, tci);

  if (expr->as.call.callee->kind != EXPR_VAR)
    return resolve_expr(tc, expr->as.call.callee, NULL);

  Symbol *sym = sym_lookup(&tci->type_syms, expr->as.call.callee->as.var.name);
  if (!sym) {
    return resolve_expr(tc, expr->as.call.callee, NULL);
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
static Type *resolve_fun_expr(TypeChecker *tc, Expr *expr, Type *hint) {
  assert(expr->kind == EXPR_CLOSURE &&
         "resolve_fun_expr: expected closure expression");
  FunDef *def = al_alloc_zero_for(tc->al, FunDef);

  def->type_param_count = expr->as.closure.type_param_count;
  def->type_params =
      al_alloc_zero(tc->al, def->type_param_count * sizeof(Type *));

  for (int i = 0; i < def->type_param_count; i++) {
    def->type_params[i] =
        ty_generic(expr->as.closure.type_params[i].name, NULL, 0, tc->al);
  }

  def->param_count = expr->as.closure.param_count;
  def->params = al_alloc_zero(tc->al, def->param_count * sizeof(ParamDef));

  for (int i = 0; i < def->param_count; i++) {
    def->params[i].name = expr->as.closure.params[i].name;
    def->params[i].is_self = expr->as.closure.params[i].is_self;
  }

  TypeArrayScratch ps;
  type_array_scratch_init(&ps, def->param_count, tc->al);

  // todo: slot assignment

  expr->as.closure.def = def;

  // check

  USE_INTERNAL(tc, tci);
  bool ok = true;

  // register type parameters as type symbols
  for (int i = 0; i < def->type_param_count; i++) {
    Symbol tp_sym = {
        .name = def->type_params[i]->as.generic.name,
        .kind = SYM_TYPE_PARAM,
        .type = def->type_params[i],
    };
    if (expr->as.closure.type_params[i].bound_count > 0) {
      tc_error(tc, expr->as.closure.type_params[i].span,
               "type parameter bounds not supported yet");
      ok = false;
    }
    sym_define(&tci->type_syms, tp_sym, tc->al);
  }

  bool has_fun_hint = hint && hint->kind == TY_FUNCTION &&
                      hint->as.fun.param_count == def->param_count;

  // resolve parameters
  for (int i = 0; i < def->param_count; i++) {
    ClosureParam *p = &expr->as.closure.params[i];
    if (p->type_annotation) {
      ps.ptr[i] = resolve_typenode(tc, p->type_annotation);
      ok = ok && !type_is_poison(ps.ptr[i]);
    } else if (has_fun_hint) {
      ps.ptr[i] = hint->as.fun.param_types[i];
    } else {
      ps.ptr[i] = ty_unknown(NULL, tc->al);
    }
  }

  // resolve return type
  Type *return_type =
      expr->as.closure.return_type_annotation
          ? resolve_typenode(tc, expr->as.closure.return_type_annotation)
      : has_fun_hint ? hint->as.fun.return_type
                     : ty_unknown(NULL, tc->al);
  def->return_type = return_type;
  ok = ok && !type_is_poison(return_type);

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
    Type *body_type = resolve_expr(tc, expr->as.closure.body, NULL);

    if (!type_is_poison(body_type) && !type_is_poison(return_type)) {
      if (!types_equal(return_type, body_type)) {
        ok = false;
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

  return ok ? ty_function(ps.ptr, ps.count, return_type, tc->al) //
            : ty_poison();
}

static void check_match_pattern(TypeChecker *tc, SubstEnv *env, Pattern *pat,
                                Type *subject_ty) {
  USE_INTERNAL(tc, tci);
  pat->resolved_type = subject_ty;

  switch (pat->kind) {
  case PAT_WILDCARD:
    break;
  case PAT_LITERAL: {
    Type *literal_ty = resolve_expr(tc, pat->as.literal_expr, NULL);
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

static Type *resolve_expr(TypeChecker *tc, Expr *expr, Type *hint) {
  (void)hint;
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
    Type *operand = resolve_expr(tc, expr->as.unary.operand, NULL);
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
    Type *lhs = resolve_expr(tc, expr->as.binary.left, NULL);
    Type *rhs = resolve_expr(tc, expr->as.binary.right, NULL);

    if (type_is_poison(lhs) || type_is_poison(rhs)) {
      result = ty_poison();
      break;
    }

    // if (lhs->kind == TY_UNKNOWN || rhs->kind == TY_UNKNOWN) {
    //   result = ty_unknown(NULL, tc->al);
    //   break;
    // }

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
      result = resolve_expr(tc, expr->as.block.tail_expr, hint);
    } else {
      result = ty_unit();
    }

    sym_pop_scope(&tci->val_syms, tc->al);
    break;
  }

  case EXPR_IF: {
    Type *cond = resolve_expr(tc, expr->as.if_expr.condition, NULL);
    if (!type_is_poison(cond) && cond->kind != TY_BOOL) {
      tc_error(tc, expr->as.if_expr.condition->span,
               "if condition must be Bool, got '%s'", type_name(cond));
    }
    Type *then_ty = resolve_expr(tc, expr->as.if_expr.then_block, NULL);
    if (expr->as.if_expr.else_branch != NULL) {
      Type *else_ty = resolve_expr(tc, expr->as.if_expr.else_branch, NULL);
      if (!type_is_poison(then_ty) && !type_is_poison(else_ty)) {
        if (!types_equal(then_ty, else_ty)) {
          char then_buf[64], else_buf[64];
          type_sprintf(then_ty, then_buf, sizeof(then_buf));
          type_sprintf(else_ty, else_buf, sizeof(else_buf));
          tc_error(tc, expr->span,
                   "if/else arms have different types: '%s' vs '%s'", then_buf,
                   else_buf);
          result = ty_poison();
        } else {
          result = then_ty;
        }
      } else {
        result = type_is_poison(then_ty) ? else_ty : then_ty;
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
        return resolve_expr(tc, expr, NULL);
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
      } else if (res.kind == PATH_RES_METHOD) {
        MethodDef *method = res.as.method.method;

        Expr new_expr = {
            .kind = EXPR_ASSOCIATED_CALL,
            .span = expr->span,
            .as.assoc_call =
                {
                    .resolved_method = method,
                    .caller = expr->as.call.callee,
                    .args = expr->as.call.args,
                    .arg_count = expr->as.call.arg_count,
                },
        };

        *expr = new_expr;
        return resolve_expr(tc, expr, NULL);
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
        assert(fun_def->type_params[i]->kind == TY_GENERIC);
        subst_bind(tc, &env, fun_def->type_params[i]->as.generic.name,
                   ty_unknown(NULL, tc->al), expr->span);
      }
    }

    bool ok = true;

    for (int i = 0; i < got; i++) {
      Type *arg_ty = resolve_expr(tc, expr->as.call.args[i],
                                  callee_type->as.fun.param_types[i]);
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

  case EXPR_METHOD_CALL: {
    Type *object_type = resolve_expr(tc, expr->as.method_call.object, NULL);
    if (type_is_poison(object_type)) {
      result = ty_poison();
      break;
    }

    if (object_type->kind != TY_STRUCT) {
      tc_error(tc, expr->span, "method call requires a struct type, got '%s'",
               type_name(object_type));
      result = ty_poison();
      break;
    }

    StructDef *def = object_type->as.struc.def;
    StringView method_name = expr->as.method_call.method_name;

    // look up the method on the struct def
    MethodDef *method = NULL;
    for (int i = 0; i < def->method_count; i++) {
      if (sv_equal(def->methods[i].name, method_name)) {
        method = &def->methods[i];
        break;
      }
    }

    if (method == NULL) {
      char obj_buf[64];
      type_sprintf(object_type, obj_buf, sizeof(obj_buf));
      tc_error(tc, expr->span, "type '%s' has no method named '" SV_FMT "'",
               obj_buf, SV_ARG(method_name));
      result = ty_poison();
      break;
    }

    expr->as.method_call.resolved_method = method;

    FunDef *fun = method->fun;
    ImplDef *impl = method->impl;

    // build substitution env: bind each struct type param to the
    // concrete type arg from the object's instantiated type
    SubstEnv env;
    subst_init(&env);

    // pre-bind impl type params to unknowns for inference
    for (int i = 0; i < impl->type_param_count; i++) {
      assert(impl->type_params[i]->kind == TY_GENERIC);
      subst_bind(tc, &env, impl->type_params[i]->as.generic.name,
                 ty_unknown(NULL, tc->al), expr->span);
    }

    // if the method's return type is generic, we can use the caller's expected
    // return type as a hint to infer it; bind the method's return type param to
    // the hint
    if (fun->return_type->kind == TY_GENERIC && hint != NULL) {
      subst_bind(tc, &env, fun->return_type->as.generic.name, hint, expr->span);
    }

    // Infer impl type params from the concrete object type.
    // impl->self_type is e.g. TY_STRUCT Person<T> (T is TY_GENERIC).
    // object_type    is e.g. TY_STRUCT Person<Int>.
    // We zip their type_args to derive T → Int.

    assert(impl->self_type->kind == TY_STRUCT);
    assert(impl->self_type->as.struc.def == object_type->as.struc.def);

    int n = impl->self_type->as.struc.type_arg_count;
    assert(n == object_type->as.struc.type_arg_count);

    for (int i = 0; i < n; i++) {
      Type *impl_arg = impl->self_type->as.struc.type_args[i]; // TY_GENERIC "T"
      Type *object_arg = object_type->as.struc.type_args[i];   // TY_INT, etc.

      if (impl_arg->kind != TY_GENERIC) {
        continue;
      }

      if (!subst_bind(tc, &env, impl_arg->as.generic.name, object_arg,
                      expr->span)) {
        Type *concrete = substitute(tc, &env, impl_arg);
        char concrete_buf[64], impl_arg_buf[64], object_arg_buf[64];
        type_sprintf(concrete, concrete_buf, sizeof(concrete_buf));
        type_sprintf(impl_arg, impl_arg_buf, sizeof(impl_arg_buf));
        type_sprintf(object_arg, object_arg_buf, sizeof(object_arg_buf));
        tc_error(tc, expr->span,
                 "cannot unify impl self type arg '%s' (was '%s') with object "
                 "type arg '%s'",
                 impl_arg_buf, concrete_buf, object_arg_buf);
      };
    }

    // also bind any method-level type params to unknowns for inference,
    // or to explicit type args if the caller supplied them
    int explicit_ty_args = expr->as.method_call.type_arg_count;
    if (explicit_ty_args > 0 && explicit_ty_args != fun->type_param_count) {
      tc_error(tc, expr->span,
               "method '" SV_FMT
               "' has %d type parameter(s) but %d were provided",
               SV_ARG(method_name), fun->type_param_count, explicit_ty_args);
      result = ty_poison();
      break;
    }
    for (int i = 0; i < fun->type_param_count; i++) {
      assert(fun->type_params[i]->kind == TY_GENERIC);
      Type *concrete =
          (explicit_ty_args > 0)
              ? resolve_typenode(tc, expr->as.method_call.type_args[i])
              : ty_unknown(NULL, tc->al);
      subst_bind(tc, &env, fun->type_params[i]->as.generic.name, concrete,
                 expr->span);
    }

    // count non-self params - the first param with is_self is provided by
    // the object, not by the argument list
    int first_value_param = 0;
    if (fun->param_count > 0 && fun->params[0].is_self) {
      first_value_param = 1;
    }
    int expected_args = fun->param_count - first_value_param;
    int got_args = expr->as.method_call.arg_count;

    if (expected_args != got_args) {
      tc_error(tc, expr->span,
               "method '" SV_FMT
               "' expects %d argument(s) but %d were provided",
               SV_ARG(method_name), expected_args, got_args);
      result = ty_poison();
      break;
    }

    bool ok = true;
    for (int i = 0; i < got_args; i++) {
      Type *param_ty =
          substitute(tc, &env, fun->params[first_value_param + i].param_type);
      Type *arg_ty = resolve_expr(tc, expr->as.method_call.args[i], param_ty);

      if (type_is_poison(arg_ty) || type_is_poison(param_ty)) {
        ok = false;
        continue;
      }

      if (!unify(tc, &env, param_ty, arg_ty,
                 expr->as.method_call.args[i]->span)) {
        Type *concrete = substitute(tc, &env, param_ty);
        if (!types_equal(concrete, arg_ty)) {
          char concrete_buf[64], arg_buf[64];
          type_sprintf(concrete, concrete_buf, sizeof(concrete_buf));
          type_sprintf(arg_ty, arg_buf, sizeof(arg_buf));
          tc_error(tc, expr->as.method_call.args[i]->span,
                   "argument %d: expected '%s', got '%s'", i + 1, concrete_buf,
                   arg_buf);
        }
        ok = false;
      }
    }

    result = ok ? substitute(tc, &env, fun->return_type) : ty_poison();
    break;
  }

  case EXPR_ASSOCIATED_CALL: {
    MethodDef *method = expr->as.assoc_call.resolved_method;
    StringView method_name = method->name;

    FunDef *fun = method->fun;
    ImplDef *impl = method->impl;

    // build substitution env: bind each struct type param to the
    // concrete type arg from the object's instantiated type
    SubstEnv env;
    subst_init(&env);
    for (int i = 0; i < impl->type_param_count; i++) {
      assert(impl->type_params[i]->kind == TY_GENERIC);
      subst_bind(tc, &env, impl->type_params[i]->as.generic.name,
                 ty_unknown(NULL, tc->al), expr->span);
    }

    // also bind any method-level type params to unknowns for inference,
    // or to explicit type args if the caller supplied them
    int explicit_ty_args = expr->as.assoc_call.type_arg_count;
    if (explicit_ty_args > 0 && explicit_ty_args != fun->type_param_count) {
      tc_error(tc, expr->span,
               "method '" SV_FMT
               "' has %d type parameter(s) but %d were provided",
               SV_ARG(method_name), fun->type_param_count, explicit_ty_args);
      result = ty_poison();
      break;
    }
    for (int i = 0; i < fun->type_param_count; i++) {
      assert(fun->type_params[i]->kind == TY_GENERIC);
      Type *concrete =
          (explicit_ty_args > 0)
              ? resolve_typenode(tc, expr->as.assoc_call.type_args[i])
              : ty_unknown(NULL, tc->al);
      subst_bind(tc, &env, fun->type_params[i]->as.generic.name, concrete,
                 expr->span);
    }

    // count non-self params — the first param with is_self is provided by
    // the object, not by the argument list
    int first_value_param = 0;
    if (fun->param_count > 0 && fun->params[0].is_self) {
      first_value_param = 1;
    }
    int expected_args = fun->param_count - first_value_param;
    int got_args = expr->as.assoc_call.arg_count;

    if (expected_args != got_args) {
      tc_error(tc, expr->span,
               "method '" SV_FMT
               "' expects %d argument(s) but %d were provided",
               SV_ARG(method_name), expected_args, got_args);
      result = ty_poison();
      break;
    }

    bool ok = true;
    for (int i = 0; i < got_args; i++) {
      Type *param_ty =
          substitute(tc, &env, fun->params[first_value_param + i].param_type);
      Type *arg_ty = resolve_expr(tc, expr->as.assoc_call.args[i], param_ty);

      if (type_is_poison(arg_ty) || type_is_poison(param_ty)) {
        ok = false;
        continue;
      }

      if (!unify(tc, &env, param_ty, arg_ty,
                 expr->as.assoc_call.args[i]->span)) {
        Type *concrete = substitute(tc, &env, param_ty);
        if (!types_equal(concrete, arg_ty)) {
          char concrete_buf[64], arg_buf[64];
          type_sprintf(concrete, concrete_buf, sizeof(concrete_buf));
          type_sprintf(arg_ty, arg_buf, sizeof(arg_buf));
          tc_error(tc, expr->as.assoc_call.args[i]->span,
                   "argument %d: expected '%s', got '%s'", i + 1, concrete_buf,
                   arg_buf);
        }
        ok = false;
      }
    }

    result = ok ? substitute(tc, &env, fun->return_type) : ty_poison();
    break;
  }

  case EXPR_ASSIGN: {
    Type *target_type = resolve_expr(tc, expr->as.assign.target, NULL);
    Type *val_type = resolve_expr(tc, expr->as.assign.value, target_type);
    if (!type_is_poison(target_type) && !type_is_poison(val_type)) {
      if (!types_equal(target_type, val_type)) {
        char target_buf[64], val_buf[64];
        type_sprintf(target_type, target_buf, sizeof(target_buf));
        type_sprintf(val_type, val_buf, sizeof(val_buf));
        tc_error(tc, expr->span,
                 "cannot assign '%s' to a variable of type '%s'", val_buf,
                 target_buf);
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
      Type *init_ty = resolve_expr(tc, fi->value, NULL);
      Type *field_ty;
      StringView fname;

      if (is_tuple) {
        field_ty = substitute(tc, &env, def->fields[i].type);
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
        field_ty = substitute(tc, &env, fdef->type);
        fname = fdef->name;
      }

      if (!check_field_type(tc, &env, field_ty, init_ty, fname, i, !is_tuple,
                            fi->span))
        ok = false;
    }

    result = ok ? substitute(tc, &env, sym->type) : ty_poison();
    break;
  }

  case EXPR_FIELD: {
    Type *target_type = resolve_expr(tc, expr->as.field.object, NULL);
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
            assert(def->type_params[i]->kind == TY_GENERIC);
            subst_bind(tc, &env, def->type_params[i]->as.generic.name, concrete,
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
      elms.ptr[i] = resolve_expr(tc, expr->as.tuple.elems[i], NULL);
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

      return resolve_expr(tc, new_expr, NULL);
    }
    case PATH_RES_METHOD: {
      tc_error(tc, expr->span,
               "method '" SV_FMT
               "' cannot be used as a value (did you forget to call it with "
               "the method call syntax 'object.method()'?)",
               SV_ARG(res.as.method.method->name));
      result = ty_poison();
      break;
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
          assert(e->type_params[i]->kind == TY_GENERIC);
          subst_bind(tc, &env, e->type_params[i]->as.generic.name,
                     ty_unknown(NULL, tc->al), callee_expr->span);
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
          assert(e->type_params[i]->kind == TY_GENERIC);
          Type *concrete =
              resolve_typenode(tc, callee_expr->as.path_expr.type_args[i]);
          subst_bind(tc, &env, e->type_params[i]->as.generic.name, concrete,
                     callee_expr->span);
        }
      }
    }

    for (int i = 0; i < v->payload_count; i++) {
      Type *arg_ty = resolve_expr(tc, expr->as.variant.payloads[i], NULL);
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
      assert(e->type_params[i]->kind == TY_GENERIC);
      Type *bound = subst_lookup(&env, e->type_params[i]->as.generic.name);
      args.ptr[i] = bound ? bound : ty_unknown(NULL, tc->al);
    }

    result = ty_enum(e, args.ptr, args.count, tc->al);
    break;
  }

  case EXPR_FOR: {
    tci->loop_depth++;
    sym_push_scope(&tci->type_syms, tc->al);

    if (expr->as.for_expr.iterable != NULL) {
      Type *iterable_type = resolve_expr(tc, expr->as.for_expr.iterable, NULL);
      if (!type_is_poison(iterable_type)) {
        tc_error(tc, expr->span,
                 "for loops over iterables not supported yet "
                 "(got type '%s')",
                 type_name(iterable_type));
      }
    }

    if (expr->as.for_expr.condition != NULL) {
      Type *cond_type = resolve_expr(tc, expr->as.for_expr.condition, NULL);
      if (!type_is_poison(cond_type) && cond_type->kind != TY_BOOL) {
        tc_error(tc, expr->as.for_expr.condition->span,
                 "for loop condition must be Bool, got '%s'",
                 type_name(cond_type));
      }
    }

    if (expr->as.for_expr.body != NULL) {
      resolve_expr(tc, expr->as.for_expr.body, NULL);
    }

    sym_pop_scope(&tci->type_syms, tc->al);
    tci->loop_depth--;

    result = ty_unit();
    break;
  }
  case EXPR_CLOSURE: {
    result = resolve_fun_expr(tc, expr, hint);
    break;
  }
  case EXPR_MATCH:
    Type *subject_ty = resolve_expr(tc, expr->as.match.subject, NULL);

    Type *result_type = NULL;

    SubstEnv env;
    subst_init(&env);

    for (int i = 0; i < expr->as.match.arm_count; i++) {
      MatchArm *arm = &expr->as.match.arms[i];

      sym_push_scope(&tci->val_syms, tc->al);

      check_match_pattern(tc, &env, arm->pattern, subject_ty);

      if (arm->guard != NULL) {
        Type *guard_ty = resolve_expr(tc, arm->guard, NULL);
        if (!type_is_poison(guard_ty) && guard_ty->kind != TY_BOOL) {
          tc_error(tc, arm->guard->span, "match guard must be Bool, got '%s'",
                   type_name(guard_ty));
        }
      }

      Type *arm_ty = resolve_expr(tc, arm->body, NULL);

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
  case EXPR_SELF: {
    Symbol *sym = sym_lookup(&tci->val_syms, sv_from_cstr("self"));
    if (!sym) {
      tc_error(tc, expr->span, "'self' is not valid in this context");
      result = ty_poison();
    } else {
      assert(sym->type && "symbol for 'self' has no type");
      result = sym->type;
    }
    break;
  }
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
      al_alloc_zero(tc->al, def->type_param_count * sizeof(Type *));

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
      al_alloc_zero(tc->al, def->type_param_count * sizeof(Type *));

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
      al_alloc_zero(tc->al, def->type_param_count * sizeof(Type *));

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

static void register_trait_decl(TypeChecker *tc, Decl *decl) {
  USE_INTERNAL(tc, tci);

  TraitDef *def = al_alloc_zero_for(tc->al, TraitDef);
  def->name = decl->as.trait_decl.name;
  def->type_param_count = decl->as.trait_decl.type_param_count;

  def->type_params =
      al_alloc_zero(tc->al, def->type_param_count * sizeof(Type *));

  TraitMethodDef scratch_methods[8];
  int method_count = 0;
  TraitAssocTypeDef scratch_assoc_types[8];
  int assoc_type_count = 0;

  for (int i = 0; i < decl->as.trait_decl.item_count; i++) {
    TraitItemNode *item = &decl->as.trait_decl.items[i];
    switch (item->kind) {
    case TRAIT_ITEM_METHOD:
      if (method_count >= 8) {
        tc_error(tc, item->span, "too many trait methods (max 8 supported)");
        break;
      }
      scratch_methods[method_count].name = item->name;
      scratch_methods[method_count].type_param_count = item->type_param_count;
      scratch_methods[method_count].type_params =
          al_alloc_zero(tc->al, item->type_param_count * sizeof(Type *));
      method_count++;
      break;
    case TRAIT_ITEM_ASSOC_TYPE:
      if (assoc_type_count >= 8) {
        tc_error(tc, item->span,
                 "too many associated types in trait (max 8 supported)");
        break;
      }
      scratch_assoc_types[assoc_type_count].name = item->name;
      assoc_type_count++;
      break;
    default:
      assert(false && "register_trait_decl: unhandled trait item kind");
      break;
    }
  }

  def->method_count = method_count;
  def->methods = al_alloc_zero(tc->al, method_count * sizeof(TraitMethodDef));
  memcpy(def->methods, scratch_methods, method_count * sizeof(TraitMethodDef));

  def->assoc_type_count = assoc_type_count;
  def->assoc_types =
      al_alloc_zero(tc->al, assoc_type_count * sizeof(TraitAssocTypeDef));
  memcpy(def->assoc_types, scratch_assoc_types,
         assoc_type_count * sizeof(TraitAssocTypeDef));

  Type *ty = ty_trait(def, tc->al);
  ty->as.trait.def = def;
  decl->as.trait_decl.def = def;

  Symbol sym = {
      .name = def->name,
      .kind = SYM_TRAIT,
      .type = ty,
      .slot = def->slot,
      .as.trait_def = def,
  };
  sym_define(&tci->type_syms, sym, tc->al);
}

static void register_impl_decl(TypeChecker *tc, Decl *decl) {
  // trait impls not supported yet
  if (decl->as.impl_decl.trait_type != NULL) {
    tc_error(tc, decl->span, "trait impls not supported yet");
    return;
  }

  ImplDef *impl_def = al_alloc_zero_for(tc->al, ImplDef);

  impl_def->type_param_count = decl->as.impl_decl.type_param_count;
  impl_def->type_params =
      al_alloc_zero(tc->al, impl_def->type_param_count * sizeof(Type *));

  decl->as.impl_decl.def = impl_def;
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
  case DECL_TRAIT:
    register_trait_decl(tc, decl);
    break;
  case DECL_IMPL:
    register_impl_decl(tc, decl);
    break;
  default:
    assert(false && "register_decl: unhandled decl kind");
    break;
  }
}

// ============================================================================
// resolve signatures
// ============================================================================

static void resolve_fun_decl(TypeChecker *tc, Decl *decl) {
  USE_INTERNAL(tc, tci);

  FunDef *def = decl->as.fun_decl.def;

  // resolve type parameters
  for (int i = 0; i < def->type_param_count; i++) {
    if (decl->as.fun_decl.type_params[i].bound_count > 0) {
      tc_error(tc, decl->as.fun_decl.type_params[i].span,
               "type parameter bounds not supported yet");
    }
    def->type_params[i] =
        ty_generic(decl->as.fun_decl.type_params[i].name, NULL, 0, tc->al);
  }

  sym_push_scope(&tci->type_syms, tc->al);

  for (int i = 0; i < def->type_param_count; i++) {
    Symbol tp_sym = {
        .name = def->type_params[i]->as.generic.name,
        .kind = SYM_TYPE_PARAM,
        .type = def->type_params[i],
    };
    sym_define(&tci->type_syms, tp_sym, tc->al);
  }

  TypeArrayScratch ps;
  type_array_scratch_init(&ps, def->param_count, tc->al);

  for (int i = 0; i < def->param_count; i++) {
    ParamDeclNode *p = &decl->as.fun_decl.params[i];
    if (p->is_self) {
      ps.ptr[i] = ty_poison();
      def->params[i].is_self = true;
    } else {
      ps.ptr[i] = resolve_typenode(tc, p->type_annotation);
      def->params[i].param_type = ps.ptr[i];
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
  def->fun_type = new_ty;

  sym_pop_scope(&tci->type_syms, tc->al);
}

static void resolve_struct_decl(TypeChecker *tc, Decl *decl) {
  USE_INTERNAL(tc, tci);
  StructDef *def = decl->as.struct_decl.def;

  // resolve type parameters
  for (int i = 0; i < def->field_count; i++) {
    if (decl->as.struct_decl.type_params[i].bound_count > 0) {
      tc_error(tc, decl->as.struct_decl.type_params[i].span,
               "type parameter bounds not supported yet");
    }
    def->type_params[i] =
        ty_generic(decl->as.struct_decl.type_params[i].name, NULL, 0, tc->al);
  }

  sym_push_scope(&tci->type_syms, tc->al);

  // register type parameters as type symbols
  for (int i = 0; i < def->type_param_count; i++) {
    Symbol tp_sym = {
        .name = def->type_params[i]->as.generic.name,
        .kind = SYM_TYPE_PARAM,
        .type = def->type_params[i],
    };
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

static void resolve_enum_decl(TypeChecker *tc, Decl *decl) {
  USE_INTERNAL(tc, tci);
  EnumDef *def = decl->as.enum_decl.def;

  // resolve type parameters
  for (int i = 0; i < def->type_param_count; i++) {
    if (decl->as.enum_decl.type_params[i].bound_count > 0) {
      tc_error(tc, decl->as.enum_decl.type_params[i].span,
               "type parameter bounds not supported yet");
    }
    def->type_params[i] =
        ty_generic(decl->as.enum_decl.type_params[i].name, NULL, 0, tc->al);
  }

  sym_push_scope(&tci->type_syms, tc->al);

  // register type parameters as type symbols
  for (int i = 0; i < def->type_param_count; i++) {
    Symbol tp_sym = {
        .name = def->type_params[i]->as.generic.name,
        .kind = SYM_TYPE_PARAM,
        .type = def->type_params[i],
    };
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

static void resolve_trait_decl(TypeChecker *tc, Decl *decl) {
  USE_INTERNAL(tc, tci);

  TraitDef *def = decl->as.trait_decl.def;

  // resolve trait type parameters
  for (int i = 0; i < def->type_param_count; i++) {
    if (decl->as.trait_decl.type_params[i].bound_count > 0) {
      tc_error(tc, decl->as.trait_decl.type_params[i].span,
               "type parameter bounds not supported yet");
    }
    def->type_params[i] =
        ty_generic(decl->as.trait_decl.type_params[i].name, NULL, 0, tc->al);
  }

  sym_push_scope(&tci->type_syms, tc->al);

  // register trait type parameters as type symbols
  for (int i = 0; i < def->type_param_count; i++) {
    Symbol tp_sym = {
        .name = def->type_params[i]->as.generic.name,
        .kind = SYM_TYPE_PARAM,
        .type = def->type_params[i],
    };
    sym_define(&tci->type_syms, tp_sym, tc->al);
  }

  def->self_type = ty_generic(sv_from_cstr("Self"), NULL, 0, tc->al);
  
  Type *saved_self = tci->current_self_type;
  tci->current_self_type = def->self_type;

  int method_idx = -1;
  for (int i = 0; i < decl->as.trait_decl.item_count; i++) {
    method_idx++;
    if (decl->as.trait_decl.items[i].kind != TRAIT_ITEM_METHOD) {
      continue;
    }

    TraitMethodDef *method = &def->methods[method_idx];
    TraitItemNode *node = &decl->as.trait_decl.items[i];

    // resolve method type parameters
    for (int i = 0; i < node->type_param_count; i++) {
      if (node->type_params[i].bound_count > 0) {
        tc_error(tc, node->type_params[i].span,
                 "type parameter bounds not supported yet");
      }
      method->type_params[i] =
          ty_generic(node->type_params[i].name, NULL, 0, tc->al);
    }

    sym_push_scope(&tci->type_syms, tc->al);

    // register method type parameters as type symbols
    for (int i = 0; i < method->type_param_count; i++) {
      Symbol tp_sym = {
          .name = method->type_params[i]->as.generic.name,
          .kind = SYM_TYPE_PARAM,
          .type = method->type_params[i],
      };
      sym_define(&tci->type_syms, tp_sym, tc->al);
    }

    TypeArrayScratch ps;
    type_array_scratch_init(&ps, 0, tc->al);

    for (int i = 0; i < node->param_count; i++) {
      if (node->params[i].is_self) {
        ps.ptr[i] = def->self_type;
      } else {
        ps.ptr[i] = resolve_typenode(tc, node->params[i].type_annotation);
      }
    }

    Type *return_type =
        node->return_type ? resolve_typenode(tc, node->return_type) : ty_unit();

    method->method_type = ty_function(ps.ptr, ps.count, return_type, tc->al);

    sym_pop_scope(&tci->type_syms, tc->al);
  }

  sym_pop_scope(&tci->type_syms, tc->al);
  tci->current_self_type = saved_self;
}

static void resolve_impl_decl(TypeChecker *tc, Decl *decl) {
  USE_INTERNAL(tc, tci);
  ImplDef *impl_def = decl->as.impl_decl.def;

  for (int i = 0; i < impl_def->type_param_count; i++) {
    if (decl->as.impl_decl.type_params[i].bound_count > 0) {
      tc_error(tc, decl->as.impl_decl.type_params[i].span,
               "type parameter bounds not supported yet");
    }
    impl_def->type_params[i] =
        ty_generic(decl->as.impl_decl.type_params[i].name, NULL, 0, tc->al);
  }

  // push impl-level type params so resolve_typenode can see them when
  sym_push_scope(&tci->type_syms, tc->al);
  for (int i = 0; i < impl_def->type_param_count; i++) {
    Symbol tp_sym = {
        .name = impl_def->type_params[i]->as.generic.name,
        .kind = SYM_TYPE_PARAM,
        .type = impl_def->type_params[i],
    };
    sym_define(&tci->type_syms, tp_sym, tc->al);
  }

  Type *self_ty = resolve_typenode(tc, decl->as.impl_decl.self_type);
  impl_def->self_type = self_ty;

  if (type_is_poison(self_ty)) {
    sym_pop_scope(&tci->type_syms, tc->al);
    return;
  }

  if (self_ty->kind != TY_STRUCT) {
    sym_pop_scope(&tci->type_syms, tc->al);
    tc_error(tc, decl->as.impl_decl.self_type->span,
             "impl target must be a struct type, got '%s'", type_name(self_ty));
    return;
  }

  StructDef *struct_def = self_ty->as.struc.def;
  Symbol self_sym = {.name = sv_from_cstr("Self"),
                     .kind = SYM_STRUCT,
                     .type = self_ty,
                     .as.struct_def = struct_def};
  sym_define(&tci->type_syms, self_sym, tc->al);

  Type *saved_self = tci->current_self_type;
  tci->current_self_type = self_ty;

  int method_count = 0;
  int accoc_type_count = 0;
  (void)accoc_type_count;

  for (int i = 0; i < decl->as.impl_decl.item_count; i++) {
    if (decl->as.impl_decl.items[i].kind == IMPL_ITEM_METHOD) {
      method_count++;
    } else if (decl->as.impl_decl.items[i].kind == IMPL_ITEM_ASSOC_TYPE) {
      accoc_type_count++;
    } else {
      assert(false && "register_impl_decl: unhandled impl item kind");
    }
  }

  if (struct_def->method_cap < method_count + struct_def->method_count) {
    int new_cap = struct_def->method_cap == 0 ? 4 : struct_def->method_cap;
    while (new_cap < method_count + struct_def->method_count) {
      new_cap *= 2;
    }
    struct_def->methods = al_realloc(tc->al, struct_def->methods,
                                     sizeof(MethodDef) * struct_def->method_cap,
                                     sizeof(MethodDef) * new_cap);
    struct_def->method_cap = new_cap;
  }

  for (int i = 0; i < decl->as.impl_decl.item_count; i++) {
    ImplItemNode *item = &decl->as.impl_decl.items[i];

    if (item->kind == IMPL_ITEM_ASSOC_TYPE) {
      tc_error(tc, item->span,
               "associated types in impl blocks not supported yet");
      continue;
    }

    assert(item->kind == IMPL_ITEM_METHOD && item->fun_decl != NULL);
    Decl *fdecl = item->fun_decl;
    assert(fdecl->kind == DECL_FUN);

    // build a stub FunDef — param types and return type filled in during check
    FunDef *fun = al_alloc_zero_for(tc->al, FunDef);
    fun->name = fdecl->as.fun_decl.name;

    fun->type_param_count = fdecl->as.fun_decl.type_param_count;
    fun->type_params =
        al_alloc_zero(tc->al, fun->type_param_count * sizeof(Type *));
    for (int j = 0; j < fun->type_param_count; j++) {
      fun->type_params[j] =
          ty_generic(fdecl->as.fun_decl.type_params[j].name, NULL, 0, tc->al);
    }

    fun->param_count = fdecl->as.fun_decl.param_count;
    fun->params = al_alloc_zero(tc->al, fun->param_count * sizeof(ParamDef));
    for (int j = 0; j < fun->param_count; j++) {
      fun->params[j].name = fdecl->as.fun_decl.params[j].name;
      fun->params[j].is_self = fdecl->as.fun_decl.params[j].is_self;
    }

    // push method-level
    sym_push_scope(&tci->type_syms, tc->al);
    for (int j = 0; j < fun->type_param_count; j++) {
      Symbol mtp_sym = {
          .name = fun->type_params[j]->as.generic.name,
          .kind = SYM_TYPE_PARAM,
          .type = fun->type_params[j],
      };
      // if (mtp->bound_count > 0) {
      //   tc_error(tc, mtp->span, "type parameter bounds not supported yet");
      // }
      sym_define(&tci->type_syms, mtp_sym, tc->al);
    }

    // resolve each parameter's type; self gets the struct's self_type
    TypeArrayScratch ps;
    type_array_scratch_init(&ps, fun->param_count, tc->al);
    for (int j = 0; j < fun->param_count; j++) {
      ParamDeclNode *p = &fdecl->as.fun_decl.params[j];
      if (p->is_self) {
        ps.ptr[j] = decl->as.impl_decl.def->self_type;
        fun->params[j].is_self = true;
      } else {
        ps.ptr[j] = resolve_typenode(tc, p->type_annotation);
      }
      fun->params[j].param_type = ps.ptr[j];
    }

    // resolve return type
    Type *return_type =
        fdecl->as.fun_decl.return_type
            ? resolve_typenode(tc, fdecl->as.fun_decl.return_type)
            : ty_unit();
    fun->return_type = return_type;

    sym_pop_scope(&tci->type_syms, tc->al);

    fdecl->as.fun_decl.def = fun;

    // duplicate method name check
    for (int j = 0; j < struct_def->method_count; j++) {
      if (sv_equal(struct_def->methods[j].name, fun->name)) {
        tc_error(tc, fdecl->span,
                 "duplicate method '" SV_FMT "' on struct '" SV_FMT "'",
                 SV_ARG(fun->name), SV_ARG(struct_def->name));
        continue;
      }
    }

    struct_def->methods[struct_def->method_count++] =
        (MethodDef){.name = fun->name, .fun = fun, .impl = impl_def};
  }

  sym_pop_scope(&tci->type_syms, tc->al);
  tci->current_self_type = saved_self;
}

static void resolve_decl(TypeChecker *tc, Decl *decl) {
  switch (decl->kind) {
  case DECL_FUN:
    resolve_fun_decl(tc, decl);
    break;
  case DECL_STRUCT:
    resolve_struct_decl(tc, decl);
    break;
  case DECL_ENUM:
    resolve_enum_decl(tc, decl);
    break;
  case DECL_TRAIT:
    resolve_trait_decl(tc, decl);
    break;
  case DECL_IMPL:
    resolve_impl_decl(tc, decl);
    break;
  default:
    assert(false && "resolve_decl: unhandled decl kind");
    break;
  }
}

// ============================================================================
// checking
// ============================================================================

static void check_fun_decl(TypeChecker *tc, Decl *decl) {
  USE_INTERNAL(tc, tci);

  FunDef *def = decl->as.fun_decl.def;
  Type *fun_ty = def->fun_type;

  if (fun_ty == NULL) {
    return;
  }

  assert(fun_ty->kind == TY_FUNCTION &&
         "check_fun_decl: function symbol has non-function type");

  Type *return_type = fun_ty->as.fun.return_type;

  sym_push_scope(&tci->type_syms, tc->al);

  for (int i = 0; i < def->type_param_count; i++) {
    Symbol tp_sym = {
        .name = def->type_params[i]->as.generic.name,
        .kind = SYM_TYPE_PARAM,
        .type = def->type_params[i],
    };
    sym_define(&tci->type_syms, tp_sym, tc->al);
  }

  FunDef *saved_fun = tci->current_fun;
  tci->current_fun = def;
  tci->loop_depth = 0;

  sym_push_scope(&tci->val_syms, tc->al);

  for (int i = 0; i < def->param_count; i++) {
    Symbol param_sym = {
        .name = def->params[i].name,
        .kind = SYM_VAR,
        .type = def->params[i].param_type,
        .slot = i,
    };
    sym_define(&tci->val_syms, param_sym, tc->al);
  }

  if (decl->as.fun_decl.body != NULL) {
    Type *body_type = resolve_expr(tc, decl->as.fun_decl.body, return_type);

    if (!type_is_poison(body_type) && !type_is_poison(return_type)) {
      if (!types_equal(body_type, return_type)) {
        char expected_buf[64], value_buf[64];
        type_sprintf(return_type, expected_buf, sizeof(expected_buf));
        type_sprintf(body_type, value_buf, sizeof(value_buf));
        Span def_span = span_subtract(decl->span, decl->as.fun_decl.body->span);
        tc_error(tc, def_span,
                 "function '" SV_FMT "' returns '%s' but body has type '%s'",
                 SV_ARG(def->name), expected_buf, value_buf);
      }
    }
  }

  sym_pop_scope(&tci->val_syms, tc->al);
  sym_pop_scope(&tci->type_syms, tc->al);

  tci->current_fun = saved_fun;
}

static void check_trait_decl(TypeChecker *tc, Decl *decl) {
  USE_INTERNAL(tc, tci);
  TraitDef *def = decl->as.trait_decl.def;

  sym_push_scope(&tci->type_syms, tc->al);

  // trait level
  for (int i = 0; i < def->type_param_count; i++) {
    Symbol sym = {
        .name = def->type_params[i]->as.generic.name,
        .kind = SYM_TYPE_PARAM,
        .type = def->type_params[i],
    };
    sym_define(&tci->type_syms, sym, tc->al);
  }

  Type *saved_self = tci->current_self_type;
  tci->current_self_type = def->self_type;

  int method_idx = -1;
  for (int i = 0; i < decl->as.trait_decl.item_count; i++) {
    method_idx++;
    if (decl->as.trait_decl.items[i].kind != TRAIT_ITEM_METHOD) {
      continue;
    }

    TraitMethodDef *method = &def->methods[method_idx];
    TraitItemNode *node = &decl->as.trait_decl.items[i];

    if (node->default_body == NULL) {
      continue;
    }

    // method level
    sym_push_scope(&tci->type_syms, tc->al);
    for (int i = 0; i < method->type_param_count; i++) {
      Symbol sym = {
          .name = method->type_params[i]->as.generic.name,
          .kind = SYM_TYPE_PARAM,
          .type = method->type_params[i],
      };
      sym_define(&tci->type_syms, sym, tc->al);
    }

    Type *ret_ty = method->method_type->as.fun.return_type;
    Type *body_ty = resolve_expr(tc, node->default_body, ret_ty);
    sym_pop_scope(&tci->val_syms, tc->al);

    if (!type_is_poison(body_ty) && !type_is_poison(ret_ty)) {
      if (!types_equal(body_ty, ret_ty)) {
        char expected_buf[64], value_buf[64];
        type_sprintf(ret_ty, expected_buf, sizeof(expected_buf));
        type_sprintf(body_ty, value_buf, sizeof(value_buf));
        tc_error(tc, node->default_body->span,
                 "default implementation of method '" SV_FMT
                 "' returns '%s' but body has type '%s'",
                 SV_ARG(method->name), expected_buf, value_buf);
      }
    }

    sym_pop_scope(&tci->type_syms, tc->al);
  }

  sym_pop_scope(&tci->type_syms, tc->al);
  tci->current_self_type = saved_self;
}

static void check_impl_decl(TypeChecker *tc, Decl *decl) {
  USE_INTERNAL(tc, tci);

  if (decl->as.impl_decl.trait_type != NULL) {
    return; // error already emitted during registration
  }

  ImplDef *def = decl->as.impl_decl.def;

  if (type_is_poison(def->self_type) || def->self_type->kind != TY_STRUCT) {
    return; // error already reported during registration
  }

  // push impl-level type params
  sym_push_scope(&tci->type_syms, tc->al);
  for (int i = 0; i < def->type_param_count; i++) {
    Symbol tp_sym = {
        .name = def->type_params[i]->as.generic.name,
        .kind = SYM_TYPE_PARAM,
        .type = def->type_params[i],
    };
    sym_define(&tci->type_syms, tp_sym, tc->al);
  }

  Type *saved_self = tci->current_self_type;
  tci->current_self_type = def->self_type;
  Symbol self_sym = {.name = sv_from_cstr("Self"),
                     .kind = SYM_STRUCT,
                     .type = tci->current_self_type,
                     .as.struct_def = tci->current_self_type->as.struc.def};
  sym_define(&tci->type_syms, self_sym, tc->al);

  for (int i = 0; i < decl->as.impl_decl.item_count; i++) {
    ImplItemNode *item = &decl->as.impl_decl.items[i];
    if (item->kind != IMPL_ITEM_METHOD || item->fun_decl == NULL) {
      continue; // assoc types already errored during registration
    }

    Decl *fdecl = item->fun_decl;
    FunDef *fun = fdecl->as.fun_decl.def;
    assert(fun != NULL && "check_impl_decl: fun_decl was not registered");

    sym_push_scope(&tci->type_syms, tc->al);
    for (int j = 0; j < fun->type_param_count; j++) {
      Symbol mtp_sym = {
          .name = fun->type_params[j]->as.generic.name,
          .kind = SYM_TYPE_PARAM,
          .type = fun->type_params[j],
      };
      sym_define(&tci->type_syms, mtp_sym, tc->al);
    }

    Type *return_type = fun->return_type;

    // type-check the body
    FunDef *saved_fun = tci->current_fun;
    tci->current_fun = fun;
    tci->loop_depth = 0;

    sym_push_scope(&tci->val_syms, tc->al);
    for (int j = 0; j < fun->param_count; j++) {
      StringView name =
          fun->params[j].is_self ? sv_from_cstr("self") : fun->params[j].name;
      Symbol param_sym = {
          .name = name,
          .kind = SYM_VAR,
          .type = fun->params[j].param_type,
          .slot = j,
      };
      sym_define(&tci->val_syms, param_sym, tc->al);
    }

    if (fdecl->as.fun_decl.body != NULL) {
      Type *body_type = resolve_expr(tc, fdecl->as.fun_decl.body, NULL);
      if (!type_is_poison(body_type) && !type_is_poison(return_type)) {
        SubstEnv env;
        subst_init(&env);

        if (!unify(tc, &env, return_type, body_type, fdecl->span)) {
          Type *concrete_expected = substitute(tc, &env, return_type);
          Type *concrete_actual = substitute(tc, &env, body_type);
          char expected_buf[64], actual_buf[64];
          type_sprintf(concrete_expected, expected_buf, sizeof(expected_buf));
          type_sprintf(concrete_actual, actual_buf, sizeof(actual_buf));
          Span def_span =
              span_subtract(fdecl->span, fdecl->as.fun_decl.body->span);
          tc_error(tc, def_span,
                   "method '" SV_FMT "' returns '%s' but body has type '%s'",
                   SV_ARG(fun->name), expected_buf, actual_buf);
        }
      }
    }

    sym_pop_scope(&tci->val_syms, tc->al);
    sym_pop_scope(&tci->type_syms, tc->al); // method-level type params

    tci->current_fun = saved_fun;
  }

  tci->current_self_type = saved_self;

  sym_pop_scope(&tci->type_syms, tc->al); // impl-level type params
}

static void check_decl(TypeChecker *tc, Decl *decl) {
  switch (decl->kind) {
  case DECL_FUN:
    check_fun_decl(tc, decl);
    break;
  case DECL_STRUCT:
  case DECL_ENUM:
    // no body to check for structs or enums
    break;
  case DECL_TRAIT:
    check_trait_decl(tc, decl);
    break;
  case DECL_IMPL:
    check_impl_decl(tc, decl);
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
  // register names
  for (int i = 0; i < program->decl_count; i++) {
    register_decl(tc, program->decls[i]);
  }

  // resolve signatures
  for (int i = 0; i < program->decl_count; i++) {
    resolve_decl(tc, program->decls[i]);
  }

  // check bodies
  for (int i = 0; i < program->decl_count; i++) {
    check_decl(tc, program->decls[i]);
  }

  USE_INTERNAL(tc, tci);
  assert(tci->type_syms.scope_count == 1 &&
         "tc_check_program: type symbol table scope count should be 1 after "
         "checking program");
  assert(tci->val_syms.scope_count == 1 &&
         "tc_check_program: value symbol table scope count should be 1 after "
         "checking program");
}