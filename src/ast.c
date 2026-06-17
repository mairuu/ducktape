#include "ast.h"
#include "allocator.h"
#include "string_utils.h"

#include <assert.h>
#include <stdio.h>

#define TYPE_INTERN_CAP 256

typedef struct {
  Type *entries[TYPE_INTERN_CAP];
  int count;
} TypeInternTable;

static TypeInternTable g_intern;

static uint32_t type_hash(const Type *t) {
  uint32_t h = (uint32_t)t->kind * 2654435761u;
  switch (t->kind) {
  case TY_GENERIC:
    for (int i = 0; i < (int)t->as.generic.name.len; i++)
      h = h * 31 + (uint8_t)t->as.generic.name.chars[i];
    for (int i = 0; i < t->as.generic.bound_count; i++)
      h = h * 31 + (uint32_t)(uintptr_t)t->as.generic.bounds[i];
    break;
  case TY_TUPLE:
    for (int i = 0; i < t->as.tuple.elem_count; i++)
      h = h * 31 + (uint32_t)(uintptr_t)t->as.tuple.elem_types[i];
    break;
  case TY_ARRAY:
    h = h * 31 + (uint32_t)(uintptr_t)t->as.array.elem_type;
    break;
  case TY_FUNCTION:
    for (int i = 0; i < t->as.fun.param_count; i++)
      h = h * 31 + (uint32_t)(uintptr_t)t->as.fun.param_types[i];
    h = h * 31 + (uint32_t)(uintptr_t)t->as.fun.return_type;
    break;
  case TY_STRUCT:
    h = h * 31 + (uint32_t)(uintptr_t)t->as.struc.def;
    for (int i = 0; i < t->as.struc.type_arg_count; i++)
      h = h * 31 + (uint32_t)(uintptr_t)t->as.struc.type_args[i];
    break;
  case TY_ENUM:
    h = h * 31 + (uint32_t)(uintptr_t)t->as.enm.def;
    for (int i = 0; i < t->as.enm.type_arg_count; i++)
      h = h * 31 + (uint32_t)(uintptr_t)t->as.enm.type_args[i];
    break;
  case TY_TRAIT:
    h = h * 31 + (uint32_t)(uintptr_t)t->as.trait.def;
    // for (int i = 0; i < t->as.trait.type_arg_count; i++)
    //   h = h * 31 + (uint32_t)(uintptr_t)t->as.trait.type_args[i];
    break;
  case TY_UNKNOWN:
    h = h * 31 + t->as.unknown.id;
    h = h * 31 + (uint32_t)(uintptr_t)t->as.unknown.bound;
    break;
  default:
    break; // singletons: kind alone is enough
  }
  return h;
}

static bool type_structurally_equal(const Type *a, const Type *b) {
  if (a->kind != b->kind)
    return false;
  switch (a->kind) {
  case TY_GENERIC:
    if (!sv_equal(a->as.generic.name, b->as.generic.name)) {
      return false;
    }
    if (a->as.generic.bound_count != b->as.generic.bound_count) {
      return false;
    }
    for (int i = 0; i < a->as.generic.bound_count; i++) {
      if (a->as.generic.bounds[i] != b->as.generic.bounds[i]) {
        return false;
      }
    }
    return true;
  case TY_TUPLE:
    if (a->as.tuple.elem_count != b->as.tuple.elem_count)
      return false;
    for (int i = 0; i < a->as.tuple.elem_count; i++)
      if (a->as.tuple.elem_types[i] != b->as.tuple.elem_types[i])
        return false;
    return true;
  case TY_ARRAY:
    return a->as.array.elem_type == b->as.array.elem_type;
  case TY_FUNCTION:
    if (a->as.fun.param_count != b->as.fun.param_count)
      return false;
    for (int i = 0; i < a->as.fun.param_count; i++)
      if (a->as.fun.param_types[i] != b->as.fun.param_types[i])
        return false;
    return a->as.fun.return_type == b->as.fun.return_type;
  case TY_STRUCT:
    if (a->as.struc.def != b->as.struc.def)
      return false;
    if (a->as.struc.type_arg_count != b->as.struc.type_arg_count)
      return false;
    for (int i = 0; i < a->as.struc.type_arg_count; i++)
      if (a->as.struc.type_args[i] != b->as.struc.type_args[i])
        return false;
    return true;
  case TY_ENUM:
    if (a->as.enm.def != b->as.enm.def)
      return false;
    if (a->as.enm.type_arg_count != b->as.enm.type_arg_count)
      return false;
    for (int i = 0; i < a->as.enm.type_arg_count; i++)
      if (a->as.enm.type_args[i] != b->as.enm.type_args[i])
        return false;
    return true;
  case TY_TRAIT:
    return a->as.trait.def == b->as.trait.def;
  default:
    return true; // singletons equal by kind
  }
}

static void sort_bounds(TraitDef **bounds, int count) {
  for (int i = 0; i < count - 1; i++) {
    for (int j = 0; j < count - i - 1; j++) {
      if (bounds[j] > bounds[j + 1]) {
        TraitDef *temp = bounds[j];
        bounds[j] = bounds[j + 1];
        bounds[j + 1] = temp;
      }
    }
  }
}

static inline bool type_is_internable(Type *t) {
  if (t->kind == TY_UNKNOWN || t->kind == TY_POISON) {
    return false;
  }
  switch (t->kind) {
    // for container
  case TY_TUPLE:
    for (int i = 0; i < t->as.tuple.elem_count; i++) {
      if (!type_is_internable(t->as.tuple.elem_types[i])) {
        return false;
      }
    }
    break;
  case TY_ARRAY:
    if (!type_is_internable(t->as.array.elem_type)) {
      return false;
    }
    break;
  case TY_STRUCT:
    for (int i = 0; i < t->as.struc.type_arg_count; i++) {
      if (!type_is_internable(t->as.struc.type_args[i])) {
        return false;
      }
    }
    break;
  case TY_ENUM:
    for (int i = 0; i < t->as.enm.type_arg_count; i++) {
      if (!type_is_internable(t->as.enm.type_args[i])) {
        return false;
      }
    }
    break;
  default:
    break;
  }
  return true;
}

static Type *type_intern(Type *t) {
  if (!type_is_internable(t)) {
    return t;
  }

  uint32_t h = type_hash(t);
  uint32_t mask = TYPE_INTERN_CAP - 1;
  uint32_t slot = h & mask;
  while (true) {
    Type *entry = g_intern.entries[slot];
    if (!entry) {
      assert(g_intern.count < TYPE_INTERN_CAP * 0.7 &&
             "intern table is too full");

      if (t->kind == TY_GENERIC) {
        sort_bounds(t->as.generic.bounds, t->as.generic.bound_count);
      }

      g_intern.entries[slot] = t;
      g_intern.count++;
      return t;
    }
    if (type_structurally_equal(entry, t))
      return entry;
    slot = (slot + 1) & mask;
  }
}

static Type *type_intern_lookup(Type *probe) {
  uint32_t h = type_hash(probe);
  uint32_t mask = TYPE_INTERN_CAP - 1;
  uint32_t slot = h & mask;
  while (g_intern.entries[slot]) {
    if (type_structurally_equal(g_intern.entries[slot], probe)) {
      return g_intern.entries[slot];
    }
    slot = (slot + 1) & mask;
  }
  return NULL; // miss
}

Type *ty_int(void) {
  static Type int_type = {.kind = TY_INT};
  return &int_type;
}

Type *ty_float(void) {
  static Type float_type = {.kind = TY_FLOAT};
  return &float_type;
}

Type *ty_bool(void) {
  static Type bool_type = {.kind = TY_BOOL};
  return &bool_type;
}

Type *ty_string(void) {
  static Type string_type = {.kind = TY_STRING};
  return &string_type;
}

Type *ty_unit(void) {
  static Type unit_type = {.kind = TY_UNIT};
  return &unit_type;
}

Type *ty_poison(void) {
  static Type poison = {.kind = TY_POISON};
  return &poison;
}

Type *ty_unknown(uint32_t id, Type *bound, Allocator *al) {
  Type *t = al_alloc_zero_for(al, Type);
  t->kind = TY_UNKNOWN;
  t->as.unknown.id = id;
  t->as.unknown.bound = bound;
  return t;
}

Type *ty_fun(Type **params, int param_count, Type *ret, Allocator *al) {
  Type probe = {.kind = TY_FUNCTION,
                .as.fun = {
                    .param_types = params,
                    .param_count = param_count,
                    .return_type = ret,
                }};
  Type *interned = type_intern_lookup(&probe);
  if (interned) {
    return interned;
  }

  Type *t = al_alloc_for(al, Type);
  t->kind = TY_FUNCTION;
  t->as.fun.param_types = al_alloc(al, param_count * sizeof(Type *));
  for (int i = 0; i < param_count; i++) {
    t->as.fun.param_types[i] = params[i];
  }
  t->as.fun.param_count = param_count;
  t->as.fun.return_type = ret;
  return type_intern(t);
}

Type *ty_tuple(Type **elems, int elem_count, Allocator *al) {
  Type probe = {.kind = TY_TUPLE,
                .as.tuple = {
                    .elem_types = elems,
                    .elem_count = elem_count,
                }};
  Type *interned = type_intern_lookup(&probe);
  if (interned) {
    return interned;
  }
  Type *t = al_alloc_for(al, Type);
  t->kind = TY_TUPLE;
  t->as.tuple.elem_types = al_alloc(al, elem_count * sizeof(Type *));
  for (int i = 0; i < elem_count; i++) {
    t->as.tuple.elem_types[i] = elems[i];
  }
  t->as.tuple.elem_count = elem_count;
  return type_intern(t);
}

// todo:
Type *ty_array(Type *elem, Allocator *al);

Type *ty_generic(StringView name, TraitDef **bounds, int bound_count,
                 Allocator *al) {
  Type *t = al_alloc_zero_for(al, Type);
  t->kind = TY_GENERIC;
  t->as.generic.name = name;
  t->as.generic.bounds = al_alloc(al, bound_count * sizeof(TraitDef *));
  for (int i = 0; i < bound_count; i++) {
    t->as.generic.bounds[i] = bounds[i];
  }
  t->as.generic.bound_count = bound_count;
  return t;
}

Type *ty_assoc(Type *base, StringView assoc_name, TraitDef *trait,
               Allocator *al) {
  Type *t = al_alloc_zero_for(al, Type);
  t->kind = TY_ASSOC;
  t->as.assoc.base = base;
  t->as.assoc.assoc_name = assoc_name;
  t->as.assoc.trait = trait;
  return t;
}

Type *ty_struct(StructDef *def, Type **args, int argc, Allocator *al) {
  Type probe = {.kind = TY_STRUCT,
                .as.struc = {
                    .def = def,
                    .type_args = args,
                    .type_arg_count = argc,
                }};
  Type *interned = type_intern_lookup(&probe);
  if (interned) {
    return interned;
  }

  Type *t = al_alloc_zero_for(al, Type);
  t->kind = TY_STRUCT;
  t->as.struc.def = def;
  t->as.struc.type_args = al_alloc(al, argc * sizeof(Type *));
  for (int i = 0; i < argc; i++) {
    t->as.struc.type_args[i] = args[i];
  }
  t->as.struc.type_arg_count = argc;
  return type_intern(t);
}

Type *ty_enum(EnumDef *def, Type **args, int argc, Allocator *al) {
  Type probe = {.kind = TY_ENUM,
                .as.enm = {
                    .def = def,
                    .type_args = args,
                    .type_arg_count = argc,
                }};
  Type *interned = type_intern_lookup(&probe);
  if (interned) {
    return interned;
  }

  Type *t = al_alloc_zero_for(al, Type);
  t->kind = TY_ENUM;
  t->as.enm.def = def;
  t->as.enm.type_args = al_alloc(al, argc * sizeof(Type *));
  for (int i = 0; i < argc; i++) {
    t->as.enm.type_args[i] = args[i];
  }
  t->as.enm.type_arg_count = argc;
  return type_intern(t);
}

Type *ty_trait(TraitDef *def, Allocator *al) {
  Type probe = {.kind = TY_TRAIT,
                .as.trait = {
                    .def = def,
                }};
  Type *interned = type_intern_lookup(&probe);
  if (interned) {
    return interned;
  }

  Type *t = al_alloc_zero_for(al, Type);
  t->kind = TY_TRAIT;
  t->as.trait.def = def;
  return type_intern(t);
}

// bool types_equal(const Type *a, const Type *b) { return a == b; }

bool type_is_numeric(const Type *t) {
  return t->kind == TY_INT || t->kind == TY_FLOAT;
}

const char *type_name(const Type *t) {
  static char buf[64];
  type_name_sprintf(t, buf, sizeof(buf));
  return buf;
}

int type_name_sprintf(const Type *t, char *buf, size_t buf_size) {
  if (!t) {
    return snprintf(buf, buf_size, "NULL_TYPE");
  }
  switch (t->kind) {
  case TY_INT:
    return snprintf(buf, buf_size, "Int");
  case TY_FLOAT:
    return snprintf(buf, buf_size, "Float");
  case TY_BOOL:
    return snprintf(buf, buf_size, "Bool");
  case TY_STRING:
    return snprintf(buf, buf_size, "String");
  case TY_UNIT:
    return snprintf(buf, buf_size, "()");
  case TY_UNKNOWN:
    if (t->as.unknown.bound) {
      return type_name_sprintf(t->as.unknown.bound, buf, buf_size);
    } else {
      return snprintf(buf, buf_size, "_");
    }
  case TY_POISON:
    return snprintf(buf, buf_size, "<POISON>");
  case TY_GENERIC:
    return snprintf(buf, buf_size, SV_FMT,
                    SV_ARG(t->as.generic.name)); // todo: include bounds
  case TY_FUNCTION:
    return snprintf(buf, buf_size, "fun(...)");
  case TY_TUPLE:
    return snprintf(buf, buf_size, "(...)");
  case TY_STRUCT:
    return snprintf(buf, buf_size, SV_FMT, SV_ARG(t->as.struc.def->name));
  case TY_ENUM:
    return snprintf(buf, buf_size, SV_FMT, SV_ARG(t->as.enm.def->name));
  case TY_TRAIT:
    return snprintf(buf, buf_size, "dyn " SV_FMT,
                    SV_ARG(t->as.trait.def->name));
  case TY_ARRAY:
    return snprintf(buf, buf_size, "Array<...>");
  case TY_ASSOC:
    return snprintf(buf, buf_size, SV_FMT "." SV_FMT,
                    SV_ARG(t->as.assoc.base->as.generic.name),
                    SV_ARG(t->as.assoc.assoc_name));
  }
  assert(false && "type_sprintf not implemented for non-singleton types");
  return 0;
}

int type_sprintf(const Type *t, char *buf, size_t buf_size) {
  if (!t) {
    return snprintf(buf, buf_size, "NULL_TYPE");
  }
  switch (t->kind) {
  case TY_INT:
    return snprintf(buf, buf_size, "Int");
  case TY_FLOAT:
    return snprintf(buf, buf_size, "Float");
  case TY_BOOL:
    return snprintf(buf, buf_size, "Bool");
  case TY_STRING:
    return snprintf(buf, buf_size, "String");
  case TY_UNIT:
    return snprintf(buf, buf_size, "()");
  case TY_UNKNOWN:
    if (t->as.unknown.bound) {
      return type_sprintf(t->as.unknown.bound, buf, buf_size);
    } else {
      return snprintf(buf, buf_size, "_");
    }
  case TY_POISON:
    return snprintf(buf, buf_size, "<POISON>");
  case TY_GENERIC:
    return snprintf(buf, buf_size, SV_FMT,
                    SV_ARG(t->as.generic.name)); // todo: include bounds
  case TY_FUNCTION: {
    int n = snprintf(buf, buf_size, "fun(");
    for (int i = 0; i < t->as.fun.param_count; i++) {
      if (i > 0)
        n += snprintf(buf + n, buf_size - n, ", ");
      n += type_sprintf(t->as.fun.param_types[i], buf + n, buf_size - n);
    }
    n += snprintf(buf + n, buf_size - n, "): ");
    return n;
  }
  case TY_TUPLE: {
    int n = snprintf(buf, buf_size, "(");
    for (int i = 0; i < t->as.tuple.elem_count; i++) {
      if (i > 0)
        n += snprintf(buf + n, buf_size - n, ", ");
      n += type_sprintf(t->as.tuple.elem_types[i], buf + n, buf_size - n);
    }
    n += snprintf(buf + n, buf_size - n, ")");
    return n;
  }
  case TY_STRUCT: {
    int n = snprintf(buf, buf_size, SV_FMT, SV_ARG(t->as.struc.def->name));
    if (t->as.struc.type_arg_count == 0) {
      return n;
    }

    bool is_tuple_struct = t->as.struc.def->is_tuple;

    n += snprintf(buf + n, buf_size - n, is_tuple_struct ? "(" : "<");
    for (int i = 0; i < t->as.struc.type_arg_count; i++) {
      if (i > 0)
        n += snprintf(buf + n, buf_size - n, ", ");
      n += type_sprintf(t->as.struc.type_args[i], buf + n, buf_size - n);
    }
    n += snprintf(buf + n, buf_size - n, is_tuple_struct ? ")" : ">");
    return n;
  }
  case TY_ENUM: {
    int n = snprintf(buf, buf_size, SV_FMT, SV_ARG(t->as.enm.def->name));
    if (t->as.enm.type_arg_count > 0) {
      n += snprintf(buf + n, buf_size - n, "<");
      for (int i = 0; i < t->as.enm.type_arg_count; i++) {
        if (i > 0)
          n += snprintf(buf + n, buf_size - n, ", ");
        n += type_sprintf(t->as.enm.type_args[i], buf + n, buf_size - n);
      }
      n += snprintf(buf + n, buf_size - n, ">");
    }
    return n;
  }
  case TY_TRAIT:
    return snprintf(buf, buf_size, "dyn " SV_FMT,
                    SV_ARG(t->as.trait.def->name)); // todo: include type args
  case TY_ARRAY:
    return snprintf(buf, buf_size, "[...]"); // todo: include elem type
  case TY_ASSOC:
    return snprintf(
        buf, buf_size, SV_FMT "." SV_FMT,
        SV_ARG(t->as.assoc.base->as.generic.name),
        SV_ARG(t->as.assoc.assoc_name)); // todo: include base type args
  }
  assert(false && "type_sprintf not implemented for non-singleton types");
  return 0;
}

Expr *ast_expr(ExprKind kind, Span span, Allocator *al) {
  Expr *e = al_alloc_zero_for(al, Expr);
  e->kind = kind;
  e->span = span;
  return e;
}

Stmt *ast_stmt(StmtKind kind, Span span, Allocator *al) {
  Stmt *s = al_alloc_zero_for(al, Stmt);
  s->kind = kind;
  s->span = span;
  return s;
}

Decl *ast_decl(DeclKind kind, Span span, Allocator *al) {
  Decl *d = al_alloc_zero_for(al, Decl);
  d->kind = kind;
  d->span = span;
  return d;
}

TypeNode *ast_type_node(TypeNodeKind kind, Span span, Allocator *al) {
  TypeNode *tn = al_alloc_zero_for(al, TypeNode);
  tn->kind = kind;
  tn->span = span;
  return tn;
}

static void ind(int indent) {
  for (int i = 0; i < indent; i++) {
    fprintf(stdout, "  ");
  }
}

static void dump_typenode(const TypeNode *tn, int indent);

static void dump_path(const Path *path, int indent) {
  for (int i = 0; i < path->count; i++) {
    ind(indent);
    fprintf(stdout, "::" SV_FMT "\n", SV_ARG(path->segments[i].name));
    for (int j = 0; j < path->segments[i].type_arg_count; j++) {
      dump_typenode(path->segments[i].type_args[j], indent + 1);
    }
  }
}

static void dump_typenode(const TypeNode *tn, int indent);
static void dump_binding_pat(const BindingPat *bp, int indent);

void dump_type(const Type *t) {
  if (!t) {
    fprintf(stdout, "NULL_TYPE");
    return;
  }
  switch (t->kind) {
  case TY_INT:
    fprintf(stdout, "Int");
    break;
  case TY_FLOAT:
    fprintf(stdout, "Float");
    break;
  case TY_BOOL:
    fprintf(stdout, "Bool");
    break;
  case TY_STRING:
    fprintf(stdout, "String");
    break;
  case TY_UNIT:
    fprintf(stdout, "()");
    break;
  case TY_UNKNOWN:
    fprintf(stdout, "<UNKNOWN>");
    break;
  case TY_POISON:
    fprintf(stdout, "<POISON>");
    break;
  case TY_GENERIC:
    fprintf(stdout, SV_FMT, SV_ARG(t->as.generic.name));
    break;
  case TY_ASSOC:
    dump_type(t->as.assoc.base);
    fprintf(stdout, "." SV_FMT, SV_ARG(t->as.assoc.assoc_name));
    break;
  case TY_ARRAY:
    fprintf(stdout, "Array<");
    dump_type(t->as.array.elem_type);
    fprintf(stdout, ">");
    break;
  case TY_TUPLE:
    fprintf(stdout, "(");
    for (int i = 0; i < t->as.tuple.elem_count; i++) {
      dump_type(t->as.tuple.elem_types[i]);
      if (i < t->as.tuple.elem_count - 1)
        fprintf(stdout, ", ");
    }
    fprintf(stdout, ")");
    break;
  case TY_FUNCTION:
    fprintf(stdout, "fun(");
    for (int i = 0; i < t->as.fun.param_count; i++) {
      dump_type(t->as.fun.param_types[i]);
      if (i < t->as.fun.param_count - 1)
        fprintf(stdout, ", ");
    }
    fprintf(stdout, "): ");
    dump_type(t->as.fun.return_type);
    break;
  case TY_STRUCT:
    fprintf(stdout, SV_FMT, SV_ARG(t->as.struc.def->name));
    if (t->as.struc.type_arg_count > 0) {
      fprintf(stdout, "<...>"); // Simplified
    }
    break;
  case TY_ENUM:
    fprintf(stdout, SV_FMT, SV_ARG(t->as.enm.def->name));
    break;
  case TY_TRAIT:
    fprintf(stdout, "dyn " SV_FMT, SV_ARG(t->as.trait.def->name));
    break;
  }
}

static void dump_typenode(const TypeNode *tn, int indent) {
  if (!tn)
    return;
  ind(indent);
  switch (tn->kind) {
  case TYNODE_UNIT:
    fprintf(stdout, "TypeNode: ()\n");
    break;
  case TYNODE_SELF:
    fprintf(stdout, "TypeNode: Self\n");
    break;
  case TYNODE_POISON:
    fprintf(stdout, "TypeNode: <POISON>\n");
    break;
  case TYNODE_NAMED:
    fprintf(stdout, "TypeNode: Named\n");
    dump_path(&tn->as.named.path, indent + 1);
    break;
  case TYNODE_TUPLE:
    fprintf(stdout, "TypeNode: Tuple\n");
    for (int i = 0; i < tn->as.tuple.count; i++) {
      dump_typenode(tn->as.tuple.elems[i], indent + 1);
    }
    break;
  case TYNODE_FUN:
    fprintf(stdout, "TypeNode: Function\n");
    ind(indent + 1);
    fprintf(stdout, "Parameters:\n");
    for (int i = 0; i < tn->as.fun.param_count; i++) {
      dump_typenode(tn->as.fun.param_types[i], indent + 2);
    }
    ind(indent + 1);
    fprintf(stdout, "Returns:\n");
    dump_typenode(tn->as.fun.return_type, indent + 2);
    break;
  case TYNODE_ASSOC:
    fprintf(stdout, "TypeNode: Assoc(" SV_FMT ")\n",
            SV_ARG(tn->as.assoc.assoc_name));
    break;
  }
  if (tn->resolved) {
    ind(indent + 1);
    fprintf(stdout, "Resolved Type:\n");
    ind(indent + 2);
    dump_type(tn->resolved);
    fprintf(stdout, "\n");
  }
}

// ═══════════════════════════════════════════════════════════════════════════════
// PATTERNS
// ═══════════════════════════════════════════════════════════════════════════════

void dump_pattern(const Pattern *p, int indent) {
  if (!p)
    return;
  ind(indent);
  switch (p->kind) {
  case PAT_WILDCARD:
    fprintf(stdout, "Pattern: _\n");
    break;
  case PAT_LITERAL:
    fprintf(stdout, "Pattern: Literal\n");
    dump_expr(p->as.literal_expr, indent + 1);
    break;
  case PAT_BIND:
    fprintf(stdout, "Pattern: Bind(" SV_FMT ")\n", SV_ARG(p->as.bind.name));
    break;
  case PAT_VARIANT:
    fprintf(stdout, "Pattern: Variant\n");
    dump_path(&p->as.variant.path, indent + 1);
    fprintf(stdout, ")\n");
    for (int i = 0; i < p->as.variant.field_count; i++) {
      ind(indent + 1);
      // fprintf(stdout, "Field: " SV_FMT "\n",
      //         SV_ARG(p->as.variant.fields[i].ident.name));
      if (p->as.variant.fields[i].sub_pattern) {
        dump_pattern(p->as.variant.fields[i].sub_pattern, indent + 2);
      }
    }
    break;
  case PAT_STRUCT:
    fprintf(stdout, "Pattern: Struct(");
    dump_path(&p->as.struc.path, indent + 1);
    fprintf(stdout, ")\n");
    for (int i = 0; i < p->as.struc.field_count; i++) {
      ind(indent + 1);
      // fprintf(stdout, "Field: " SV_FMT "\n",
      //         SV_ARG(p->as.struc.fields[i].ident.name));
      if (p->as.struc.fields[i].sub_pattern) {
        dump_pattern(p->as.struc.fields[i].sub_pattern, indent + 2);
      }
    }
    break;
  case PAT_TUPLE:
    fprintf(stdout, "Pattern: Tuple\n");
    for (int i = 0; i < p->as.tuple.count; i++) {
      dump_pattern(p->as.tuple.elems[i], indent + 1);
    }
    break;
  }
}

static void dump_binding_pat(const BindingPat *bp, int indent) {
  ind(indent);
  switch (bp->kind) {
  case BIND_IDENT:
    fprintf(stdout, "Bind: " SV_FMT "\n", SV_ARG(bp->as.ident));
    break;
  case BIND_TUPLE:
    fprintf(stdout, "BindTuple: (");
    for (int i = 0; i < bp->as.tuple.count; i++) {
      fprintf(stdout, SV_FMT, SV_ARG(bp->as.tuple.names[i]));
      if (i < bp->as.tuple.count - 1)
        fprintf(stdout, ", ");
    }
    fprintf(stdout, ")\n");
    break;
  case BIND_STRUCT:
    fprintf(stdout, "BindStruct: ");
    dump_path(&bp->as.struc.path, indent + 1);
    fprintf(stdout, "\n");
    break;
  case BIND_POISON:
    fprintf(stdout, "Bind: <POISON>\n");
    break;
  }
}

// ═══════════════════════════════════════════════════════════════════════════════
// EXPRESSIONS
// ═══════════════════════════════════════════════════════════════════════════════

static void dump_bound(const TraitBound *bound, int indent) {
  if (bound->ref_count == 0)
    return;
  ind(indent);
  fprintf(stdout, "Bound:\n");
  for (int i = 0; i < bound->ref_count; i++) {
    ind(indent + 1);
    fprintf(stdout, "Ref: ");
    dump_path(&bound->refs[i].path, indent + 2);
  }
}

static void dump_where_clause(const WhereClause *where, int indent) {
  if (!where || where->pred_count == 0)
    return;
  ind(indent);
  fprintf(stdout, "Where Clause:\n");
  for (int i = 0; i < where->pred_count; i++) {
    ind(indent + 1);
    fprintf(stdout, "Predicate: ");
    for (int j = 0; j < where->preds[i].lhs.segment_count; j++) {
      fprintf(stdout, SV_FMT, SV_ARG(where->preds[i].lhs.segments[j]));
      if (j < where->preds[i].lhs.segment_count - 1)
        fprintf(stdout, ".");
    }
    fprintf(stdout, "\n");
    dump_bound(&where->preds[i].bound, indent + 2);
  }
}

void dump_expr(const Expr *e, int indent) {
  if (!e)
    return;
  ind(indent);

  switch (e->kind) {
  case EXPR_INT:
    fprintf(stdout, "Int: %ld\n", e->as.int_val);
    break;
  case EXPR_FLOAT:
    fprintf(stdout, "Float: %f\n", e->as.float_val);
    break;
  case EXPR_BOOL:
    fprintf(stdout, "Bool: %s\n", e->as.bool_val ? "true" : "false");
    break;
  case EXPR_STRING:
    fprintf(stdout, "String: \"" SV_FMT "\"\n", STR_ARG(e->as.string.value));
    break;
  case EXPR_UNIT:
    fprintf(stdout, "Unit: ()\n");
    break;
  case EXPR_POISON:
    fprintf(stdout, "<POISON EXPR>\n");
    break;
  case EXPR_SELF:
    fprintf(stdout, "Self\n");
    break;
  // case EXPR_VAR:
  //   fprintf(stdout, "Var: " SV_FMT "\n", SV_ARG(e->as.var.name));
  //   break;
  case EXPR_PATH:
    fprintf(stdout, "Path:\n");
    dump_path(&e->as.path_expr.path, indent + 1);
    break;
  case EXPR_BINARY:
    fprintf(stdout, "BinaryOp (%s)\n", token_type_to_string(e->as.binary.op));
    dump_expr(e->as.binary.left, indent + 1);
    dump_expr(e->as.binary.right, indent + 1);
    break;
  case EXPR_UNARY:
    fprintf(stdout, "UnaryOp (%s)\n", token_type_to_string(e->as.unary.op));
    dump_expr(e->as.unary.operand, indent + 1);
    break;
  case EXPR_ASSIGN:
    fprintf(stdout, "Assign\n");
    ind(indent + 1);
    fprintf(stdout, "Operator: %s\n", token_type_to_string(e->as.assign.op));
    dump_expr(e->as.assign.target, indent + 1);
    dump_expr(e->as.assign.value, indent + 1);
    break;
  case EXPR_CALL:
    fprintf(stdout, "Call\n");
    dump_expr(e->as.call.callee, indent + 1);
    ind(indent + 1);
    fprintf(stdout, "Arguments:\n");
    for (int i = 0; i < e->as.call.arg_count; i++) {
      dump_expr(e->as.call.args[i], indent + 2);
    }
    break;
  case EXPR_METHOD_CALL:
    fprintf(stdout, "MethodCall: ." SV_FMT "()\n",
            SV_ARG(e->as.method_call.method_name));
    dump_expr(e->as.method_call.object, indent + 1);
    for (int i = 0; i < e->as.method_call.arg_count; i++) {
      dump_expr(e->as.method_call.args[i], indent + 2);
    }
    break;
  case EXPR_ASSOCIATED_CALL:
    fprintf(stdout, "AssociatedCall: \n");
    break;
  case EXPR_BLOCK:
    fprintf(stdout, "Block\n");
    for (int i = 0; i < e->as.block.stmt_count; i++) {
      dump_stmt(e->as.block.stmts[i], indent + 1);
    }
    if (e->as.block.tail_expr) {
      dump_expr(e->as.block.tail_expr, indent + 1);
    }
    break;
  case EXPR_IF:
    fprintf(stdout, "If\n");
    ind(indent + 1);
    fprintf(stdout, "Condition:\n");
    dump_expr(e->as.if_expr.condition, indent + 2);
    ind(indent + 1);
    fprintf(stdout, "Then:\n");
    dump_expr(e->as.if_expr.then_block, indent + 2);
    if (e->as.if_expr.else_branch) {
      ind(indent + 1);
      fprintf(stdout, "Else:\n");
      dump_expr(e->as.if_expr.else_branch, indent + 2);
    }
    break;
  case EXPR_MATCH:
    fprintf(stdout, "Match\n");
    dump_expr(e->as.match.subject, indent + 1);
    for (int i = 0; i < e->as.match.arm_count; i++) {
      ind(indent + 1);
      fprintf(stdout, "Arm:\n");
      dump_pattern(e->as.match.arms[i].pattern, indent + 2);
      if (e->as.match.arms[i].guard) {
        ind(indent + 2);
        fprintf(stdout, "Guard:\n");
        dump_expr(e->as.match.arms[i].guard, indent + 3);
      }
      ind(indent + 2);
      fprintf(stdout, "Body:\n");
      dump_expr(e->as.match.arms[i].body, indent + 3);
    }
    break;
  case EXPR_FIELD: {
    StringView field_name =
        e->as.field.is_tuple ? (StringView){} : e->as.field.ident.name;
    fprintf(stdout, "Field Access: ." SV_FMT "\n", SV_ARG(field_name));
    dump_expr(e->as.field.object, indent + 1);
    break;
  }
  case EXPR_STRUCT_INIT:
    fprintf(stdout, "StructInit:\n");
    dump_path(&e->as.struct_init.path, indent + 1);
    for (int i = 0; i < e->as.struct_init.field_count; i++) {
      ind(indent + 1);
      fprintf(stdout, "Field: " SV_FMT "\n",
              SV_ARG(e->as.struct_init.fields[i].ident.name));
      dump_expr(e->as.struct_init.fields[i].value, indent + 2);
    }
    break;
  case EXPR_TUPLE:
    fprintf(stdout, "Tuple\n");
    for (int i = 0; i < e->as.tuple.count; i++) {
      dump_expr(e->as.tuple.elems[i], indent + 1);
    }
    break;
  case EXPR_FOR: {
    fprintf(stdout, "For\n");
    ind(indent + 1);
    fprintf(stdout, "Variable: " SV_FMT "\n", SV_ARG(e->as.for_expr.var_name));
    ind(indent + 1);
    fprintf(stdout, "Iterable:\n");
    dump_expr(e->as.for_expr.iterable, indent + 2);
    ind(indent + 1);
    fprintf(stdout, "Body:\n");
    dump_expr(e->as.for_expr.body, indent + 2);
    break;
  }
  case EXPR_WHILE: {
    fprintf(stdout, "While\n");
    ind(indent + 1);
    fprintf(stdout, "Condition:\n");
    dump_expr(e->as.while_expr.condition, indent + 2);
    ind(indent + 1);
    fprintf(stdout, "Body:\n");
    dump_expr(e->as.while_expr.body, indent + 2);
    break;
  }
  case EXPR_RANGE:
    fprintf(stdout, "Range %s\n",
            e->as.range.inclusive ? "(inclusive)" : "(exclusive)");
    dump_expr(e->as.range.start, indent + 1);
    dump_expr(e->as.range.end, indent + 1);
    break;
  case EXPR_INTERPOLATED: {
    fprintf(stdout, "Interpolated String\n");
    for (int i = 0; i < e->as.interpolated.seg_count; i++) {
      if (e->as.interpolated.segs[i].kind == ISEG_EXPR) {
        ind(indent + 1);
        fprintf(stdout, "Expr Segment:\n");
        dump_expr(e->as.interpolated.segs[i].expr, indent + 2);
      } else {
        ind(indent + 1);
        fprintf(stdout, "String Segment: \"" SV_FMT "\"\n",
                STR_ARG(e->as.interpolated.segs[i].text));
      }
    }
    break;
  }
  case EXPR_INDEX: {
    fprintf(stdout, "Index\n");
    ind(indent + 1);
    fprintf(stdout, "Object:\n");
    dump_expr(e->as.index.object, indent + 2);
    ind(indent + 1);
    fprintf(stdout, "Index:\n");
    dump_expr(e->as.index.index, indent + 2);
    break;
  }
  case EXPR_CAST: {
    fprintf(stdout, "Cast\n");
    ind(indent + 1);
    fprintf(stdout, "Operand:\n");
    dump_expr(e->as.cast.operand, indent + 2);
    ind(indent + 1);
    fprintf(stdout, "Target Type:\n");
    dump_typenode(e->as.cast.target_type, indent + 2);
    break;
  }
  case EXPR_PROPAGATE: {
    fprintf(stdout, "Propagate\n");
    ind(indent + 1);
    fprintf(stdout, "Operand:\n");
    dump_expr(e->as.propagate.operand, indent + 2);
    break;
  }
  // case EXPR_ARRAY:
  case EXPR_VARIANT:
    fprintf(stdout, "Variant: ");
    dump_path(&e->as.variant.path, indent + 1);
    fprintf(stdout, "\n");
    // for (int i = 0; i < e->as.variant.payload_count; i++) {
    //   dump_expr(e->as.variant.payloads[i], indent + 1);
    // }
    break;
  case EXPR_CLOSURE:
    fprintf(stdout, "Closure\n");
    for (int i = 0; i < e->as.closure.param_count; i++) {
      ind(indent + 1);
      fprintf(stdout, "Param: " SV_FMT "\n",
              SV_ARG(e->as.closure.params[i].name));
      if (e->as.closure.params[i].type_annotation) {
        dump_typenode(e->as.closure.params[i].type_annotation, indent + 2);
      }
    }
    if (e->as.closure.return_type_annotation) {
      ind(indent + 1);
      fprintf(stdout, "Returns:\n");
      dump_typenode(e->as.closure.return_type_annotation, indent + 2);
    }
    ind(indent + 1);
    fprintf(stdout, "Body:\n");
    dump_expr(e->as.closure.body, indent + 2);
    break;
  default:
    fprintf(stdout, "ExprKind: %d (Add remaining cases as needed)\n", e->kind);
    break;
  }
}

void dump_stmt(const Stmt *s, int indent) {
  if (!s)
    return;
  ind(indent);
  switch (s->kind) {
  case STMT_EXPR:
    fprintf(stdout, "ExprStmt\n");
    dump_expr(s->as.expr_stmt.expr, indent + 1);
    break;
  case STMT_VAR:
    fprintf(stdout, "VarStmt\n");
    dump_binding_pat(&s->as.var_stmt.binding, indent + 1);
    if (s->as.var_stmt.type_annotation) {
      dump_typenode(s->as.var_stmt.type_annotation, indent + 1);
    }
    if (s->as.var_stmt.initializer) {
      dump_expr(s->as.var_stmt.initializer, indent + 1);
    }
    break;
  case STMT_RETURN:
    fprintf(stdout, "ReturnStmt\n");
    if (s->as.return_stmt.value) {
      dump_expr(s->as.return_stmt.value, indent + 1);
    }
    break;
  case STMT_BREAK:
    fprintf(stdout, "BreakStmt\n");
    break;
  case STMT_CONTINUE:
    fprintf(stdout, "ContinueStmt\n");
    break;
  case STMT_POISON:
    fprintf(stdout, "<POISON STMT>\n");
    break;
  }
}

void dump_decl(const Decl *d, int indent) {
  if (!d)
    return;
  ind(indent);
  switch (d->kind) {
  case DECL_FUN:
    fprintf(stdout, "FunDecl: " SV_FMT "\n", SV_ARG(d->as.fun_decl.name));
    for (int i = 0; i < d->as.fun_decl.type_param_count; i++) {
      ind(indent + 1);
      fprintf(stdout, "TypeParam: " SV_FMT "\n",
              SV_ARG(d->as.fun_decl.type_params[i].name));
      dump_bound(&d->as.fun_decl.type_params[i].inline_bound, indent + 2);
    }
    for (int i = 0; i < d->as.fun_decl.param_count; i++) {
      ind(indent + 1);
      fprintf(stdout, "Param: " SV_FMT "\n",
              SV_ARG(d->as.fun_decl.params[i].name));
      dump_typenode(d->as.fun_decl.params[i].type_annotation, indent + 2);
    }
    if (d->as.fun_decl.return_type) {
      ind(indent + 1);
      fprintf(stdout, "Returns:\n");
      dump_typenode(d->as.fun_decl.return_type, indent + 2);
    }
    dump_where_clause(d->as.fun_decl.where_clause, indent + 1);
    if (d->as.fun_decl.body) {
      dump_expr(d->as.fun_decl.body, indent + 1);
    }
    break;

  case DECL_STRUCT:
    fprintf(stdout, "StructDecl: " SV_FMT "\n", SV_ARG(d->as.struct_decl.name));
    if (d->as.struct_decl.is_tuple) {
      for (int i = 0; i < d->as.struct_decl.field_count; i++) {
        dump_typenode(d->as.struct_decl.fields[i].type_annotation, indent + 1);
      }
    } else {
      for (int i = 0; i < d->as.struct_decl.field_count; i++) {
        ind(indent + 1);
        fprintf(stdout, "Field: " SV_FMT "\n",
                SV_ARG(d->as.struct_decl.fields[i].ident.name));
        dump_typenode(d->as.struct_decl.fields[i].type_annotation, indent + 2);
      }
    }
    break;

  case DECL_ENUM:
    fprintf(stdout, "EnumDecl: " SV_FMT "\n", SV_ARG(d->as.enum_decl.name));
    for (int i = 0; i < d->as.enum_decl.variant_count; i++) {
      ind(indent + 1);
      fprintf(stdout, "Variant: " SV_FMT "\n",
              SV_ARG(d->as.enum_decl.variants[i].name));
      if (d->as.enum_decl.variants[i].field_count == 0) {
        continue; // No payload
      }
      if (d->as.enum_decl.variants[i].is_tuple) {
        for (int j = 0; j < d->as.enum_decl.variants[i].field_count; j++) {
          dump_typenode(d->as.enum_decl.variants[i].fields[j].type_annotation,
                        indent + 2);
        }
      } else {
        for (int j = 0; j < d->as.enum_decl.variants[i].field_count; j++) {
          ind(indent + 2);
          fprintf(stdout, "Field: " SV_FMT "\n",
                  SV_ARG(d->as.enum_decl.variants[i].fields[j].ident.name));
          dump_typenode(d->as.enum_decl.variants[i].fields[j].type_annotation,
                        indent + 3);
        }
      }
    }
    break;

  case DECL_USE:
    fprintf(stdout, "UseDecl:\n");

    ind(indent + 1);
    printf("Path:\n");
    dump_path(&d->as.use_decl.path, indent + 2);

    ind(indent + 1);
    printf("Aliases:\n");
    for (int i = 0; i < d->as.use_decl.target.count; i++) {
      ind(indent + 2);
      fprintf(stdout, SV_FMT " as " SV_FMT "\n",
              SV_ARG(d->as.use_decl.target.aliases[i].name),
              SV_ARG(d->as.use_decl.target.aliases[i].alias));
    }
    break;

  case DECL_VAR:
    fprintf(stdout, "VarDecl\n");
    dump_binding_pat(&d->as.var_decl.binding, indent + 1);
    if (d->as.var_decl.initializer) {
      dump_expr(d->as.var_decl.initializer, indent + 1);
    }
    break;

  case DECL_TRAIT:
    fprintf(stdout, "TraitDecl: " SV_FMT "\n", SV_ARG(d->as.trait_decl.name));
    for (int i = 0; i < d->as.trait_decl.type_param_count; i++) {
      ind(indent + 1);
      fprintf(stdout, "TypeParam: " SV_FMT "\n",
              SV_ARG(d->as.trait_decl.type_params[i].name));
      dump_bound(&d->as.trait_decl.type_params[i].inline_bound, indent + 2);
    }
    for (int i = 0; i < d->as.trait_decl.item_count; i++) {
      ind(indent + 2);
      fprintf(stdout, "Item: " SV_FMT "\n",
              SV_ARG(d->as.trait_decl.items[i].name));

      ind(indent + 3);
      if (d->as.trait_decl.items[i].kind == TRAIT_ITEM_METHOD) {
        fprintf(stdout, "MethodDecl: %s\n",
                d->as.trait_decl.items[i].default_body ? "with default impl"
                                                       : "");

        if (d->as.trait_decl.items[i].type_param_count > 0) {
          ind(indent + 4);
          fprintf(stdout, "Type Parameters:\n");
          for (int j = 0; j < d->as.trait_decl.items[i].type_param_count; j++) {
            ind(indent + 5);
            fprintf(stdout, "TypeParam: " SV_FMT "\n",
                    SV_ARG(d->as.trait_decl.items[i].type_params[j].name));
            dump_bound(&d->as.trait_decl.items[i].type_params[j].inline_bound,
                       indent + 6);
          }
        }

        if (d->as.trait_decl.items[i].param_count > 0) {
          ind(indent + 4);
          fprintf(stdout, "Parameters:\n");
          for (int j = 0; j < d->as.trait_decl.items[i].param_count; j++) {

            if (d->as.trait_decl.items[i].params[j].is_self) {
              ind(indent + 5);
              fprintf(stdout, "Param: self\n");
            } else {
              ind(indent + 5);
              fprintf(stdout, "Param: " SV_FMT "\n",
                      SV_ARG(d->as.trait_decl.items[i].params[j].name));
              dump_typenode(d->as.trait_decl.items[i].params[j].type_annotation,
                            indent + 6);
            }
          }
        }
      } else {
        fprintf(stdout, "Associated Type: " SV_FMT "\n",
                SV_ARG(d->as.trait_decl.items[i].name));
      }
    }
    break;

  case DECL_IMPL:
    fprintf(stdout, "ImplDecl\n");
    ind(indent + 1);
    fprintf(stdout, "For:\n");
    dump_typenode(d->as.impl_decl.self_type, indent + 2);
    if (d->as.impl_decl.type_param_count > 0) {
      ind(indent + 1);
      fprintf(stdout, "Type Parameters:\n");
      for (int i = 0; i < d->as.impl_decl.type_param_count; i++) {
        ind(indent + 2);
        fprintf(stdout, "TypeParam: " SV_FMT "\n",
                SV_ARG(d->as.impl_decl.type_params[i].name));
        dump_bound(&d->as.impl_decl.type_params[i].inline_bound, indent + 3);
      }
    }
    ind(indent + 1);
    fprintf(stdout, "Impls Trait:\n");
    dump_typenode(d->as.impl_decl.trait_type, indent + 2);
    if (d->as.impl_decl.item_count > 0) {
      ind(indent + 1);
      fprintf(stdout, "Items:\n");
      for (int i = 0; i < d->as.impl_decl.item_count; i++) {
        ind(indent + 2);
        fprintf(stdout, "Item: " SV_FMT "\n",
                SV_ARG(d->as.impl_decl.items[i].name));
        if (d->as.impl_decl.items[i].kind == IMPL_ITEM_METHOD) {
          dump_decl(d->as.impl_decl.items[i].fun_decl, indent + 3);
        } else {
          ind(indent + 3);
          fprintf(stdout, "Associated Type: " SV_FMT "\n",
                  SV_ARG(d->as.impl_decl.items[i].name));
          dump_typenode(d->as.impl_decl.items[i].assoc_type, indent + 4);
        }
      }
    }
    break;

  case DECL_POISON:
    fprintf(stdout, "<POISON DECL>\n");
    break;
  }
}

void dump_program(const Program *p, int indent) {
  if (!p)
    return;
  ind(indent);
  fprintf(stdout, "Program:\n");
  for (int i = 0; i < p->decl_count; i++) {
    dump_decl(p->decls[i], indent + 1);
  }
}