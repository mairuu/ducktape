#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "allocator.h"
#include "scanner.h"
#include "string_utils.h"

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

typedef struct Module Module;
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
  TY_NEVER,    // !    — diverging code (return/break); coerces to any type
  TY_FUNCTION, // fun(A, B): R

  TY_UNKNOWN,

  TY_TUPLE,   // (A, B, C)
  TY_STRUCT,  // back-pointer to StructDef
  TY_ENUM,    // back-pointer to EnumDef
  TY_TRAIT,   // trait object / bound
  TY_ARRAY,   // Array<T>
  TY_RANGE,   // a..b / a..=b — Int-only for now
  TY_GENERIC, // unresolved type parameter, e.g. T
  TY_ASSOC,   // T.Item   —  associated-type access before resolution
  TY_POISON,  // sentinel: error already reported, suppress downstream errors
} TypeKind;

// Type union variants
typedef struct {
  Type **param_types;
  int param_count;
  Type *return_type;
} TypeFun;

typedef struct {
  Type **elem_types;
  int elem_count;
} TypeTuple;

typedef struct {
  StructDef *def;
  Type **type_args;
  int type_arg_count;
} TypeStruct;

typedef struct {
  EnumDef *def;
  Type **type_args;
  int type_arg_count;
} TypeEnum;

typedef struct {
  TraitDef *def;
  // Type **type_args;
  // int type_arg_count;
} TypeTrait;

typedef struct {
  Type *elem_type;
} TypeArray;

// TY_GENERIC — an unresolved type parameter like T
typedef struct {
  StringView name;   // e.g. "T"
  TraitDef **bounds; // e.g. [Display, Clone]
  int bound_count;
} TypeGeneric;

// TY_ASSOC — T.Item before resolution
typedef struct {
  Type *base; // the T in T.Item
  TraitDef *trait;
  StringView assoc_name; // the "Item" in T.Item
} TypeAssoc;

typedef struct {
  uint32_t id;
  Type *bound;
  StringView param_name; // for diagnostics
  Span intro_span;       // for diagnostics
} TypeUnknown;

struct Type {
  TypeKind kind;
  union {
    TypeFun fun;
    TypeTuple tuple;
    TypeStruct struc;
    TypeEnum enm;
    TypeTrait trait;
    TypeArray array;
    TypeGeneric generic;
    TypeAssoc assoc;
    TypeUnknown unknown;
  } as;
};

Type *ty_int(void);
Type *ty_float(void);
Type *ty_bool(void);
Type *ty_string(void);
Type *ty_unit(void);
Type *ty_never(void);
Type *ty_range(void);
Type *ty_poison(void);

Type *ty_unknown(uint32_t id, Type *bound, Allocator *al);
Type *ty_fun(Type **params, int param_count, Type *ret, Allocator *al);
Type *ty_tuple(Type **elems, int elem_count, Allocator *al);
Type *ty_array(Type *elem, Allocator *al);
Type *ty_generic(StringView name, TraitDef **bounds, int bound_count,
                 Allocator *al);
Type *ty_assoc(Type *base, StringView assoc_name, TraitDef *trait,
               Allocator *al);
Type *ty_struct(StructDef *def, Type **args, int argc, Allocator *al);
Type *ty_enum(EnumDef *def, Type **args, int argc, Allocator *al);
Type *ty_trait(TraitDef *def, Allocator *al);

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

typedef union {
  StringView name;
  int index;
} FieldIdent;

typedef struct {
  FieldIdent ident;
  Type *type; // resolved field type
} FieldDef;

typedef struct {
  StringView name;
  FunDef *fun;
} MethodDef;

struct StructDef {
  Module *module;
  bool is_pub;

  Type *self_type;

  StringView name;

  Type **type_params;   // e.g. ["T", "U"]
  int type_param_count; // if generic, else 0

  FieldDef *fields;
  int field_count;
  bool is_tuple;

  int slot;
};

typedef struct {
  StringView name;
  FieldDef *fields;
  int field_count;
  bool is_tuple;
  uint8_t tag;
} VariantDef;

struct EnumDef {
  Module *module;
  bool is_pub;

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
  Type **type_params;
  int type_param_count;
  bool has_default;
  FunDef *default_impl; // NULL if required
} TraitMethodDef;

typedef struct {
  StringView name;
  Type *type;
} AssocTypeDef;

struct TraitDef {
  Module *module;
  bool is_pub;

  StringView name;

  Type *self_type;

  Type **type_params;
  int type_param_count;

  TraitMethodDef *methods;
  int method_count;

  AssocTypeDef *assoc_types;
  int assoc_type_count;

  int slot;
};

struct ImplDef {
  Module *module;

  Type *trait_type; // NULL => inherent impl
  Type *self_type;

  Type **type_params;
  int type_param_count;

  MethodDef *methods;
  int method_count;
  int method_cap;

  AssocTypeDef *assoc_types;
  int assoc_type_count;
  int assoc_type_cap;

  int slot;
};

typedef struct {
  StringView name;
  Type *param_type;
  bool is_self; // implicit "self"
} ParamDef;

struct FunDef {
  Module *module;
  bool is_pub;

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

typedef struct {
  StringView name;
  TypeNode **type_args;
  int type_arg_count; // 0 for non-generic segments
} PathSegment;

struct Path {
  PathSegment *segments;
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
  TYNODE_ARRAY, // [T]
  TYNODE_FUN,   // fun(A, B): R
  TYNODE_SELF,  // Self
  TYNODE_ASSOC, // T.Item
  TYNODE_POISON // sentinel
} TypeNodeKind;

// TypeNode union variants
typedef struct {
  Path path;
} TypeNodeNamed;

typedef struct {
  TypeNode **elems;
  int count;
} TypeNodeTuple;

typedef struct {
  TypeNode **param_types;
  int param_count;
  TypeNode *return_type;
} TypeNodeFun;

typedef struct {
  TypeNode *base;
  StringView assoc_name;
} TypeNodeAssoc;

typedef struct {
  TypeNode *elem;
} TypeNodeArray;

struct TypeNode {
  TypeNodeKind kind;
  Span span;
  Type *resolved; // filled in by type checker; NULL util then

  union {
    TypeNodeNamed named;
    TypeNodeTuple tuple;
    TypeNodeFun fun;
    TypeNodeAssoc assoc;
    TypeNodeArray array;
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
  FieldIdent ident;
  Pattern *sub_pattern; // NULL means shorthand, bind same name as field
  Span span;
} FieldPat;

// Pattern union variants
typedef struct {
  StringView name;
} PatternBind;

typedef struct {
  Path path;
  FieldPat *fields;
  int field_count;
} PatternVariant;

typedef struct {
  Path path;
  FieldPat *fields;
  int field_count;
} PatternStruct;

typedef struct {
  Pattern **elems;
  int count;
} PatternTuple;

struct Pattern {
  PatternKind kind;
  Span span;
  Type *resolved_type;

  union {
    Expr *literal_expr;
    PatternBind bind;
    PatternVariant variant;
    PatternStruct struc;
    PatternTuple tuple;
  } as;
};

// ═══════════════════════════════════════════════════════════════════════════════
// TRAIT REFERENCES AND WHERE CLAUSES
// ═══════════════════════════════════════════════════════════════════════════════

// A single trait in a bound, optionally generic: Clone, Iterator<Int>, From<T>
typedef struct {
  Path path; // the trait name, possibly qualified: std::fmt::Display
  Span span;
} TraitRef;

// One or more trait refs joined by +: Clone + Iterator<Int> + From<T>
typedef struct {
  TraitRef *refs;
  int ref_count; // always >= 1
} TraitBound;

typedef struct {
  StringView *segments; // e.g. {"T"}  or  {"T", "ID"}
  int segment_count;    // always >= 1
  Span span;
} WhereLhs;

// A single predicate inside a where clause: T: Clone + Display
typedef struct {
  WhereLhs lhs;
  TraitBound bound;
  Span span;
} WherePred;

// The whole where clause: where T: Clone, U: Into<String>
// NULL pointer on any decl means no where clause was written.
typedef struct {
  WherePred *preds;
  int pred_count; // always >= 1 when the node exists
  Span span;
} WhereClause;

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
  FieldIdent ident;
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
  TraitBound
      inline_bound; // refs == NULL / ref_count == 0 means no inline bound
  Span span;
} TypeParamNode;

typedef enum { ISEG_TEXT, ISEG_EXPR } InterpolSegKind;
typedef struct {
  InterpolSegKind kind;
  StringView text; // (slice into source for ISEG_TEXT)
  Expr *expr;      // ISEG_EXPR
} InterpolSeg;

// Expr union variants
typedef struct {
  StringView value;
} ExprString;

typedef struct {
  InterpolSeg *segs;
  int seg_count;
} ExprInterpolated;

// typedef struct {
//   StringView name;
//   int resolved_slot; // -1 until resolver runs
//   bool is_upvalue;
//   int upvalue_index;
// } ExprVar;

typedef struct {
  Path path;
  // TypeNode **type_args;
  // int type_arg_count;
} ExprPath;

typedef struct {
  Expr *left;
  Expr *right;
  TokenType op;
} ExprBinary;

typedef struct {
  Expr *operand;
  TokenType op;
} ExprUnary;

typedef struct {
  Expr *target;
  Expr *value;
  TokenType op;
} ExprAssign;

typedef struct {
  Expr *start;
  Expr *end;
  bool inclusive; // true for ..=, false for ..
} ExprRange;

typedef struct {
  Expr *callee;
  Expr **args;
  int arg_count;
} ExprCall;

typedef struct {
  Expr *object;
  Expr *index;
} ExprIndex;

typedef struct {
  Expr *object;
  FieldIdent ident;
  bool is_tuple;
  int resolved_index;
} ExprField;

typedef struct {
  Expr *object;
  StringView method_name;
  Expr **args;
  int arg_count;
  TypeNode **type_args;
  int type_arg_count;
  MethodDef *resolved_method;
  ImplDef *resolved_impl;
} ExprMethodCall;

typedef struct {
  Expr **args;
  int arg_count;
  TypeNode **type_args;
  int type_arg_count;
  MethodDef *resolved_method;
  ImplDef *resolved_impl;
  Expr *caller;
} ExprAssocCall;

typedef struct {
  Expr *operand;
  TypeNode *target_type;
} ExprCast;

typedef struct {
  Expr *operand;
} ExprPropagate;

typedef struct {
  Stmt **stmts;
  int stmt_count;
  Expr *tail_expr;
} ExprBlock;

typedef struct {
  Expr *condition;
  Expr *then_block;
  Expr *else_branch;
} ExprIf;

typedef struct {
  StringView var_name;
  Span var_span;
  Expr *iterable;
  Expr *body;
} ExprFor;

typedef struct {
  Expr *condition;
  Expr *body;
} ExprWhile;

typedef struct {
  Expr *subject;
  MatchArm *arms;
  int arm_count;
  bool enforce_exhaustiveness;
} ExprMatch;

typedef struct {
  Expr **elems;
  int count;
} ExprTuple;

typedef struct {
  Expr **elems;
  int count;
} ExprArray;

typedef struct {
  Path path;
  FieldInit *fields;
  int field_count;
  Type *resolved_struct; // NULL until resolver runs
} ExprStructInit;

typedef struct {
  Path path;
  FieldInit *fields;
  int field_count;
  VariantDef *resolved_variant; // NULL until resolver runs
  Type *resolved_enum;          // NULL until resolver runs
} ExprVariant;

typedef struct {
  ClosureParam *params;
  int param_count;
  TypeNode *return_type_annotation; // NULL if inferred
  Expr *body;                       // EXPR_BLOCK or shorthand
  FunDef *def;
} ExprClosure;

struct Expr {
  ExprKind kind;
  Type *resolved_type;
  Span span;

  union {
    int64_t int_val;
    double float_val;
    bool bool_val;
    ExprString string;

    ExprInterpolated interpolated;

    // ExprVar var;

    ExprPath path_expr;

    ExprBinary binary;

    ExprUnary unary;

    ExprAssign assign;

    ExprRange range;

    ExprCall call;

    ExprIndex index;

    ExprField field;

    ExprMethodCall method_call;

    ExprAssocCall assoc_call;

    ExprCast cast;

    ExprPropagate propagate;

    ExprBlock block;

    ExprIf if_expr;

    ExprFor for_expr;

    ExprWhile while_expr;

    ExprMatch match;

    ExprTuple tuple;

    ExprArray array;

    ExprStructInit struct_init;

    ExprVariant variant;

    ExprClosure closure;
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

// BindingPat union variants
typedef struct {
  StringView *names;
  int count;
} BindingPatTuple;

typedef struct {
  Path path;
  StringView *field_names;
  int field_count;
} BindingPatStruct;

typedef struct {
  BindKind kind;
  Span span;

  union {
    StringView ident;
    BindingPatTuple tuple;
    BindingPatStruct struc;
  } as;
} BindingPat;

// Stmt union variants
typedef struct {
  Expr *expr;
} StmtExpr;

typedef struct {
  BindingPat binding;
  TypeNode *type_annotation; // NULL if inferred
  Expr *initializer;
} StmtVar;

typedef struct {
  Expr *value; // NULL for bare "return;"
} StmtReturn;

struct Stmt {
  StmtKind kind;
  Span span;

  union {
    StmtExpr expr_stmt;
    StmtVar var_stmt;
    StmtReturn return_stmt;
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
  union {
    StringView name;
    int index; // for tuple structs
  } ident;
  TypeNode *type_annotation;
  Span span;
} FieldDeclNode;

typedef struct {
  StringView name;
  FieldDeclNode *fields;
  int field_count;
  bool is_tuple;
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

// Decl union variants
typedef struct {
  Path path;
  UseTarget target;
} DeclUse;

typedef struct {
  StringView name;
  TypeParamNode *type_params;
  int type_param_count;
  WhereClause *where_clause; // NULL if no where clause

  FieldDeclNode *fields;
  int field_count;
  bool is_tuple;

  StructDef *def;
} DeclStruct;

typedef struct {
  StringView name;
  TypeParamNode *type_params;
  int type_param_count;
  WhereClause *where_clause;

  VariantDeclNode *variants;
  int variant_count;

  EnumDef *def;
} DeclEnum;

typedef struct {
  StringView name;
  TypeParamNode *type_params;
  int type_param_count;
  WhereClause *where_clause;
  TraitItemNode *items;
  int item_count;
  TraitDef *def;
} DeclTrait;

typedef struct {
  TypeParamNode *type_params;
  int type_param_count;
  TypeNode *self_type;
  TypeNode *trait_type;
  WhereClause *where_clause;
  ImplItemNode *items;
  int item_count;

  ImplDef *def;
  // // todo: create ImplDef
  // Type *resolved_self_type;
} DeclImpl;

typedef struct {
  StringView name;
  TypeParamNode *type_params;
  int type_param_count;
  ParamDeclNode *params;
  int param_count;
  TypeNode *return_type;     // NULL -> unit
  WhereClause *where_clause; // NULL if no where clause
  bool shorthand;            // => expr; form
  Expr *body;                // EXPR_BLOCK or shorthand
  // resolved:
  FunDef *def;
} DeclFun;

typedef struct {
  BindingPat binding;
  TypeNode *type_annotation;
  Expr *initializer;
} DeclVar;

struct Decl {
  DeclKind kind;
  bool is_pub;
  Span span;
  union {
    DeclUse use_decl;
    DeclStruct struct_decl;
    DeclEnum enum_decl;
    DeclTrait trait_decl;
    DeclImpl impl_decl;
    DeclFun fun_decl;
    DeclVar var_decl;
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