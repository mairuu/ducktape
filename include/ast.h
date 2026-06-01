#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "allocator.h"
#include "scanner.h"
#include "string_utils.h"

typedef struct {
  int line;
  int line_end;
  int col;
  int col_end;
} Span;

// ───────────────────────────────────────────────────────────────────────────────
// Forward declarations
// ───────────────────────────────────────────────────────────────────────────────

typedef struct Type Type;
typedef struct Expr Expr;
typedef struct Stmt Stmt;
typedef struct Decl Decl;
typedef struct Pattern Pattern;
typedef struct TypeNode TypeNode;
typedef struct Path Path;

typedef struct StructDef StructDef;
typedef struct EnumDef EnumDef;
typedef struct TraitDef TraitDef;
typedef struct ImplDef ImplDef;
typedef struct FunDef FunDef;

// ═══════════════════════════════════════════════════════════════════════════════
// TYPE SYSTEM
// ═══════════════════════════════════════════════════════════════════════════════

typedef enum {
  TY_INT,
  TY_FLOAT,

  TY_BOOL,
  TY_STRING,
  TY_UNIT,     // ()   — functions that return nothing, empty blocks
  TY_FUNCTION, // fun(A, B): R

  TY_UNKNOWN,

  TY_TUPLE,   // (A, B, C)
  TY_STRUCT,  // back-pointer to StructDef
  TY_ENUM,    // back-pointer to EnumDef
  TY_TRAIT,   // trait object / bound
  TY_ARRAY,   // Array<T>
  TY_GENERIC, // unresolved type parameter, e.g. T
  TY_ASSOC,   // T.Item   —  associated-type access before resolution
  TY_POISON,  // sentinel: error already reported, suppress downstream errors
} TypeKind;

struct Type {
  TypeKind kind;
  union {
    struct {
      Type **param_types;
      int param_count;
      Type *return_type;
    } fun;

    struct {
      Type **elem_types;
      int elem_count;
    } tuple;

    struct {
      StructDef *def;
      Type **type_args;
      int type_arg_count;
    } struc;

    struct {
      EnumDef *def;
      Type **type_args;
      int type_arg_count;
    } enm;

    struct {
      TraitDef *def;
      Type **type_args;
      int type_arg_count;
    } trait;

    struct {
      Type *elem_type;
    } array;

    // TY_GENERIC — an unresolved type parameter like T
    struct {
      StringView name;   // e.g. "T"
      TraitDef **bounds; // e.g. [Display, Clone]
      int bound_count;
    } generic;

    // TY_ASSOC — T.Item before resolution
    struct {
      Type *base;            // the T in T.Item
      StringView assoc_name; // the "Item" in T.Item
    } assoc;

    struct {
      uint32_t id;
      Type *bound;
    } unknown;
  } as;
};

Type *ty_int(void);
Type *ty_float(void);
Type *ty_bool(void);
Type *ty_string(void);
Type *ty_unit(void);
Type *ty_poison(void);

Type *ty_unknown(Type *bound, Allocator *al);
Type *ty_function(Type **params, int param_count, Type *ret, Allocator *al);
Type *ty_tuple(Type **elems, int elem_count, Allocator *al);
Type *ty_array(Type *elem, Allocator *al);
Type *ty_generic(StringView name, TraitDef **bounds, int bound_count,
                 Allocator *al);
Type *ty_assoc(Type *base, StringView assoc_name, Allocator *al);
Type *ty_struct(StructDef *def, Type **args, int argc, Allocator *al);
Type *ty_enum(EnumDef *def, Type **args, int argc, Allocator *al);
Type *ty_trait(TraitDef *def, Type **args, int argc, Allocator *al);

static inline bool types_equal(const Type *a, const Type *b) { return a == b; }
bool type_is_numeric(const Type *t);
static inline bool type_is_poison(const Type *t) {
  return t->kind == TY_POISON;
}

const char *
type_name(const Type *t); // shared underlying char buffer, not thread safe
int type_name_sprintf(const Type *t, char *buf, size_t buf_size);
int type_sprintf(const Type *t, char *buf,
                 size_t buf_size); // fully qualified, with type args

// ═══════════════════════════════════════════════════════════════════════════════
// DEFINITION TABLES
// ═══════════════════════════════════════════════════════════════════════════════

typedef struct {
  StringView name;
  Type *type; // resolved field type
} FieldDef;

typedef struct {
  StringView name;
  FunDef *fun;
  ImplDef *impl;
} MethodDef;

struct StructDef {
  Type *self_type;
  StringView name;
  bool is_tuple_struct;

  Type **type_params; // e.g. ["T", "U"]
  int type_param_count;    // if generic, else 0

  FieldDef *fields;
  int field_count;

  MethodDef *methods; // for inherent impls
  int method_count;
  int method_cap;

  int slot;
};

// only supports tuple variants
typedef struct {
  StringView name;
  Type **payload_types;
  int payload_count; // for tuple variants, else 0
  uint8_t tag;
} VariantDef;

struct EnumDef {
  Type *self_type;
  StringView name;

  Type **type_params;
  int type_param_count;

  VariantDef *variants;
  int variant_count;

  int slot;
};

typedef struct {
  StringView name;
  Type *method_type; // TY_FUNCTION with Self still unresolved
  bool has_default;
  FunDef *default_impl; // NULL if required
} TraitMethodDef;

typedef struct {
  StringView name;
} TraitAssocTypeDef;

struct TraitDef {
  StringView name;
  Type *self_type;

  StringView *type_params;
  int type_param_count;

  TraitMethodDef *methods;
  int method_count;

  TraitAssocTypeDef *assoc_types;
  int assoc_type_count;

  int slot;
};

struct ImplDef {
  StringView name; // for error messages only; always empty for inherent impls

  // Type *trait; // NULL for inherent impls
  // Type *self_type;

  Type **type_params;
  int type_param_count;

  Type *self_type;

  // MethodDef *methods;
  // int method_count;
  // int method_cap;

  // TraitAssocTypeDef *assoc_types;
  // int assoc_type_count;
};

typedef struct {
  StringView name;
  Type *param_type;
  bool is_self; // implicit "self"
} ParamDef;

struct FunDef {
  StringView name;
  bool is_closure;
  Type *fun_type;

  Type **type_params;
  int type_param_count;

  ParamDef *params;
  int param_count;
  Type *return_type;

  // struct Chunk *chunk;
  int slot;
};

// ═══════════════════════════════════════════════════════════════════════════════
// PATH
// ═══════════════════════════════════════════════════════════════════════════════

struct Path {
  StringView *segments; // e.g. ["std", "io", "File"]
  int count;
  Span span;
};

// ═══════════════════════════════════════════════════════════════════════════════
// TYPE NODES  (syntactic — produced by parser, resolved by type checker)
// ═══════════════════════════════════════════════════════════════════════════════

typedef enum {
  TYNODE_UNIT,  // ()
  TYNODE_NAMED, // Int, MyStruct, Option<T>
  TYNODE_TUPLE, // (A, B)
  TYNODE_FUN,   // fun(A, B): R
  TYNODE_SELF,  // Self
  TYNODE_ASSOC, // T.Item
  TYNODE_POISON // sentinel
} TypeNodeKind;

struct TypeNode {
  TypeNodeKind kind;
  Span span;
  Type *resolved; // filled in by type checker; NULL util then

  union {
    // TYNODE_NAMED
    struct {
      Path path;
      TypeNode **type_args;
      int type_arg_count;
    } named;

    // TYNODE_TUPLE
    struct {
      TypeNode **elems;
      int count;
    } tuple;

    // TYNODE_FUN
    struct {
      TypeNode **param_types;
      int param_count;
      TypeNode *return_type;
    } fun;

    // TYNODE_ASSOC: T.Item
    struct {
      TypeNode *base;
      StringView assoc_name;
    } assoc;
  } as;
};

// ═══════════════════════════════════════════════════════════════════════════════
// PATTERNS
// ═══════════════════════════════════════════════════════════════════════════════

typedef enum {
  PAT_WILDCARD, // _
  PAT_LITERAL,  // 1, true, "hello"
  PAT_BIND,     // x                      — variable binding
  PAT_VARIANT,  // Some(x), None, Ok(a, b)
  PAT_STRUCT,   // Point { x, y }
  PAT_TUPLE,    // (a, b)
} PatternKind;

typedef struct {
  StringView field_name;
  Pattern *sub_pattern; // NULL means shorthand, bind same name as field
  Span span;
} FieldPat;

struct Pattern {
  PatternKind kind;
  Span span;
  Type *resolved_type;

  union {
    // PAT_LITERAL
    Expr *literal_expr;

    // PAT_BIND
    struct {
      StringView name;
    } bind;

    // PAT_VARIANT
    struct {
      Path path;
      Pattern **payloads;
      int payload_count;
      VariantDef *resolved_variant;
    } variant;

    // PAT_STRUCT
    struct {
      Path path;
      FieldPat *fields;
      int field_count;
    } struc;

    // PAT_TUPLE
    struct {
      Pattern **elems;
      int count;
    } tuple;
  } as;
};

// ═══════════════════════════════════════════════════════════════════════════════
// EXPRESSIONS
// ═══════════════════════════════════════════════════════════════════════════════

typedef enum {
  // ── Literals (pass 1) ─────────────────────────────────
  EXPR_INT,    // 42
  EXPR_FLOAT,  // 3.14
  EXPR_BOOL,   // true / false
  EXPR_STRING, // "hello {name}"
  EXPR_UNIT,   // ()

  // ── Variables / paths (pass 1+) ───────────────────────
  EXPR_VAR,  // x    — local/global name
  EXPR_SELF, // self
  EXPR_PATH, // std::io::File  — module-qualified name

  // ── Operators (pass 1+) ───────────────────────────────
  EXPR_BINARY, // a + b, a == b, a and b
  EXPR_UNARY,  // -x, not x
  EXPR_ASSIGN, // x = expr, x += expr
  EXPR_RANGE,  // a..b, a..=b

  // ── Postfix (pass 1+) ─────────────────────────────────
  EXPR_CALL,            // f(a, b)
  EXPR_INDEX,           // arr[i]
  EXPR_FIELD,           // obj.field
  EXPR_METHOD_CALL,     // obj.method(args)
  EXPR_ASSOCIATED_CALL, // T::method(args)
  EXPR_CAST,            // expr as Type
  EXPR_PROPAGATE,       // expr?              — Result propagation

  // ── Structural (pass 2+) ──────────────────────────────
  EXPR_BLOCK, // { stmts... expr? }
  EXPR_IF,    // if cond { } else { }
  EXPR_WHILE, // while cond { }
  EXPR_FOR,   // for x in iter { }
  EXPR_MATCH, // match expr { arms }
  // EXPR_RETURN, // return expr? (also appears as Stmt; duplicated for
  // expression
  //              // contexts)

  // ── Constructors (pass 3) ─────────────────────────────
  EXPR_TUPLE,       // (a, b, c)
  EXPR_ARRAY,       // [a, b, c]
  EXPR_STRUCT_INIT, // Point { x: 1, y: 2 }
  EXPR_VARIANT,     // Some(x), None
  EXPR_CLOSURE,     // fun(x: Int): Int { x + 1 }

  // ── String interpolation segment (pass 3 QoL) ─────────
  EXPR_INTERPOLATED, // "hello {name}" broken into segments

  EXPR_POISON, // sentinel
} ExprKind;

typedef struct {
  Pattern *pattern;
  Expr *guard; // NULL if no guard
  Expr *body;
  Span span;
} MatchArm;

typedef struct {
  StringView name;
  Expr *value;
  Span span;
} FieldInit;

typedef struct {
  StringView name;
  TypeNode *type_annotation;
  bool is_self;
  Span span;
} ClosureParam;

typedef struct {
  StringView name;
  StringView *bounds; // trait names as strings, resolved later
  int bound_count;
  Span span;
} TypeParamNode;

typedef enum { ISEG_TEXT, ISEG_EXPR } InterpolSegKind;
typedef struct {
  InterpolSegKind kind;
  StringView text; // (slice into source for ISEG_TEXT)
  Expr *expr;      // ISEG_EXPR
} InterpolSeg;

struct Expr {
  ExprKind kind;
  Type *resolved_type;
  Span span;

  union {
    // EXPR_INT
    int64_t int_val;
    // EXPR_FLOAT
    double float_val;
    //  EXPR_BOOL
    bool bool_val;

    // EXPR_STRING — owned because escape processing allocates
    struct {
      StringView value;
    } string;

    // EXPR_INTERPOLATED
    struct {
      InterpolSeg *segs;
      int seg_count;
    } interpolated;

    // EXPR_VAR
    struct {
      StringView name;
      int resolved_slot; // -1 until resolver runs
      bool is_upvalue;
      int upvalue_index;
    } var;

    // EXPR_PATH
    struct {
      Path path;
      TypeNode **type_args;
      int type_arg_count;
    } path_expr;

    // EXPR_BINARY; op is TOKEN_PLUS, TOKEN_MINUS, TOKEN_EQEQ, etc.
    struct {
      Expr *left;
      Expr *right;
      TokenType op;
    } binary;

    // EXPR_UNARY; op is TOKEN_MINUS or TOKEN_NOT
    struct {
      Expr *operand;
      TokenType op;
    } unary;

    // EXPR_ASSIGN; op is TOKEN_EQ, TOKEN_PLUS_EQ, TOKEN_MINUS_EQ, etc.
    struct {
      Expr *target;
      Expr *value;
      TokenType op;
    } assign;

    // EXPR_RANGE; op distinguishes .. from ..=
    struct {
      Expr *start;
      Expr *end;
      bool inclusive; // true for ..=, false for ..
    } range;

    // EXPR_CALL
    struct {
      Expr *callee;
      Expr **args;
      int arg_count;
    } call;

    // EXPR_INDEX
    struct {
      Expr *object;
      Expr *index;
    } index;

    // EXPR_FIELD
    struct {
      Expr *object;
      bool is_tuple_field;
      int tuple_index;       // if is_tuple_field
      StringView field_name; // if !is_tuple_field
      int resolved_index;
    } field;

    // EXPR_METHOD_CALL
    struct {
      Expr *object;
      StringView method_name;
      Expr **args;
      int arg_count;
      TypeNode **type_args;
      int type_arg_count;
      MethodDef *resolved_method;
    } method_call;

    // EXPR_ASSOCIATED_CALL
    struct {
      Expr **args;
      int arg_count;
      TypeNode **type_args;
      int type_arg_count;
      MethodDef *resolved_method;
      Expr *caller;
    } assoc_call;

    // EXPR_CAST
    struct {
      Expr *operand;
      TypeNode *target_type;
    } cast;

    // EXPR_PROPAGATE
    struct {
      Expr *operand;
    } propagate;

    // EXPR_BLOCK
    struct {
      Stmt **stmts;
      int stmt_count;
      Expr *tail_expr;
    } block;

    // EXPR_IF
    struct {
      Expr *condition;
      Expr *then_block;
      Expr *else_branch;
    } if_expr;

    // EXPR_WHILE
    // struct {
    //   Expr *condition;
    //   Expr *body;
    // } while_expr;

    // EXPR_FOR
    struct {
      StringView var_name;
      Expr *iterable;
      Expr *condition;
      Expr *body;
      bool is_while; // true if this is a while loop desugared into for
    } for_expr;

    // EXPR_MATCH
    struct {
      Expr *subject;
      MatchArm *arms;
      int arm_count;
      bool enforce_exhaustiveness;
    } match;

    // EXPR_TUPLE
    struct {
      Expr **elems;
      int count;
    } tuple;

    // EXPR_ARRAY
    struct {
      Expr **elems;
      int count;
    } array;

    // EXPR_STRUCT_INIT
    struct {
      Path path;
      TypeNode **type_args;
      int type_arg_count;
      FieldInit *fields;
      int field_count;
      StructDef *resolved_def; // NULL until resolver runs
    } struct_init;

    // EXPR_VARIANT
    struct {
      Path path;
      Expr *caller;
      Expr **payloads;
      int payload_count;
      VariantDef *resolved_variant; // NULL until resolver runs
      EnumDef *resolved_enum;       // NULL until resolver runs
    } variant;

    // EXPR_CLOSURE
    struct {
      TypeParamNode *type_params;
      int type_param_count;
      ClosureParam *params;
      int param_count;
      TypeNode *return_type_annotation; // NULL if inferred
      Expr *body;                       // EXPR_BLOCK or shorthand
      FunDef *def;
    } closure;
  } as;
};

// ═══════════════════════════════════════════════════════════════════════════════
// STATEMENTS
// ═══════════════════════════════════════════════════════════════════════════════

typedef enum {
  STMT_EXPR,     // expr ;
  STMT_VAR,      // var x: T = expr ;
  STMT_RETURN,   // return expr? ;
  STMT_BREAK,    // break ;
  STMT_CONTINUE, // continue ;
  STMT_POISON,   // sentinel
} StmtKind;

typedef enum {
  BIND_IDENT,  // var x = ...
  BIND_TUPLE,  // var (x, y) = ...
  BIND_STRUCT, // var Point { x, y } = ...
  BIND_POISON, // sentinel
} BindKind;

typedef struct {
  BindKind kind;
  Span span;

  union {
    StringView ident;

    struct {
      StringView *names;
      int count;
    } tuple;

    struct {
      StringView struct_name;
      StringView *field_names;
      int field_count;
    } struc;
  } as;
} BindingPat;

struct Stmt {
  StmtKind kind;
  Span span;

  union {
    // STMT_EXPR
    struct {
      Expr *expr;
    } expr_stmt;

    // STMT_VAR
    struct {
      BindingPat binding;
      TypeNode *type_annotation; // NULL if inferred
      Expr *initializer;
    } var_stmt;

    // STMT_RETURN
    struct {
      Expr *value; // NULL for bare "return;"
    } return_stmt;
  } as;
};

// ═══════════════════════════════════════════════════════════════════════════════
// DECLARATIONS
// ═══════════════════════════════════════════════════════════════════════════════

typedef enum {
  DECL_USE,
  DECL_STRUCT,
  DECL_ENUM,
  DECL_TRAIT,
  DECL_IMPL,
  DECL_FUN,
  DECL_VAR,
  DECL_POISON, // sentinel: error already reported, suppress downstream errors
} DeclKind;

typedef struct {
  StringView name;
  StringView alias; // nullable (length == 0)
} UseAlias;

typedef struct {
  UseAlias *aliases;
  int count;
} UseTarget;

typedef struct {
  StringView name;
  TypeNode *type_annotation;
  Span span;
} FieldDeclNode;

typedef struct {
  StringView name;
  TypeNode **payload_types;
  int payload_count;
  Span span;
} VariantDeclNode;

typedef struct {
  StringView name;
  TypeNode *type_annotation; // NULL if 'self'
  bool is_self;
  Span span;
} ParamDeclNode;

typedef enum { TRAIT_ITEM_ASSOC_TYPE, TRAIT_ITEM_METHOD } TraitItemKind;
typedef struct {
  TraitItemKind kind;
  StringView name;

  // TRAIT_ITEM_METHOD
  TypeParamNode *type_params;
  int type_param_count;
  ParamDeclNode *params;
  int param_count;
  TypeNode *return_type; // NULL -> unit
  Expr *default_body;    // NULL -> required

  // TRAIT_ITEM_ACSOC_TYPE
  // no additional fields (only name)

  Span span;
} TraitItemNode;

typedef enum { IMPL_ITEM_ASSOC_TYPE, IMPL_ITEM_METHOD } ImplItemKind;
typedef struct {
  ImplItemKind kind;
  StringView name;
  TypeNode *assoc_type; // IMPL_ITEM_ASSOC_TYPE
  Decl *fun_decl;       // IMPL_ITEM_METHOD
  Span span;
} ImplItemNode;

struct Decl {
  DeclKind kind;
  bool is_pub;
  Span span;
  union {
    // DECL_USE
    struct {
      Path path;
      UseTarget target;
    } use_decl;

    // DECL_STRUCT
    struct {
      StringView name;
      TypeParamNode *type_params;
      int type_param_count;
      bool is_tuple_struct;

      // c-style fields (is_tuple_struct == false)
      FieldDeclNode *fields;
      int field_count;

      // tuple struct fields (is_tuple_struct == true)
      TypeNode **tuple_types;
      int tuple_type_count;

      // resolved:
      StructDef *def;
    } struct_decl;

    // DECL_ENUM
    struct {
      StringView name;
      TypeParamNode *type_params;
      int type_param_count;
      VariantDeclNode *variants;
      int variant_count;
      EnumDef *def;
    } enum_decl;

    // DECL_TRAIT
    struct {
      StringView name;
      TypeParamNode *type_params;
      int type_param_count;
      TraitItemNode *items;
      int item_count;
      TraitDef *def;
    } trait_decl;

    // DECL_IMPL
    struct {
      TypeParamNode *type_params;
      int type_param_count;
      TypeNode *self_type;
      TypeNode *trait_type;
      ImplItemNode *items;
      int item_count;

      ImplDef *def;
      // // todo: create ImplDef
      // Type *resolved_self_type;
    } impl_decl;

    // DECL_FUN
    struct {
      StringView name;
      TypeParamNode *type_params;
      int type_param_count;
      ParamDeclNode *params;
      int param_count;
      TypeNode *return_type; // NULL -> unit
      bool shorthand;        // => expr; form
      Expr *body;            // EXPR_BLOCK or shorthand
      // resolved:
      FunDef *def;
    } fun_decl;

    // DECL_VAR (top-level only)
    struct {
      BindingPat binding;
      TypeNode *type_annotation;
      Expr *initializer;
    } var_decl;
  } as;
};

// ═══════════════════════════════════════════════════════════════════════════════
// PROGRAM ROOT
// ═══════════════════════════════════════════════════════════════════════════════

typedef struct {
  Decl **decls;
  int decl_count;
} Program;

// ═══════════════════════════════════════════════════════════════════════════════
// Helper
// ═══════════════════════════════════════════════════════════════════════════════

Expr *ast_expr(ExprKind kind, Span span, Allocator *al);
Stmt *ast_stmt(StmtKind kind, Span span, Allocator *al);
Decl *ast_decl(DeclKind kind, Span span, Allocator *al);
TypeNode *ast_type_node(TypeNodeKind kind, Span span, Allocator *al);

// ═══════════════════════════════════════════════════════════════════════════════
// DEBUG
// ═══════════════════════════════════════════════════════════════════════════════

void dump_program(const Program *p, int indent);
void dump_decl(const Decl *d, int indent);
void dump_expr(const Expr *e, int indent);
void dump_stmt(const Stmt *s, int indent);
void dump_pattern(const Pattern *p, int indent);
void dump_type(const Type *t);