#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "allocator.h"
#include "native.h"
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

typedef struct Obj Obj; // object.h; only ever a `singleton` below

// sema.h; the binding a name resolved to, recorded on the nodes that declare
// or read one so a later pass need do no name resolution of its own.
typedef struct VarEntry VarEntry;

typedef struct Module Module;
typedef struct StructDef StructDef;
typedef struct EnumDef EnumDef;
typedef struct VariantDef VariantDef;
typedef struct TraitDef TraitDef;
typedef struct TraitMethodDef TraitMethodDef;
typedef struct ImplDef ImplDef;
typedef struct FunDef FunDef;
typedef struct Subst Subst;

// ═══════════════════════════════════════════════════════════════════════════════
// TYPE SYSTEM
// ═══════════════════════════════════════════════════════════════════════════════

typedef enum {
  TY_INT,
  TY_FLOAT,

  TY_BOOL,
  TY_CHAR, // a Unicode scalar value; a string is bytes, so the two are
           // bridged by a conversion and never by an index
  TY_STRING,
  TY_STRBUF,   // StringBuf — a growable text buffer; see include/object.h
  TY_BYTES,    // Bytes     — a packed byte buffer; see include/object.h
  TY_UNIT,     // ()   — functions that return nothing, empty blocks
  TY_NEVER,    // !    — diverging code (return/break); coerces to any type
  TY_FUNCTION, // fun(A, B): R

  TY_UNKNOWN,

  TY_TUPLE,   // (A, B, C)
  TY_STRUCT,  // back-pointer to StructDef
  TY_ENUM,    // back-pointer to EnumDef
  TY_TRAIT,   // the abstract `Self` of a trait: bound position, default bodies
  TY_DYN,     // `dyn Trait` — a trait object: a value plus its vtable
  TY_ARRAY,   // Array<T>
  TY_RANGE,   // a..b / a..=b — int-only for now
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

// TY_TRAIT — a trait *reference*: the trait and the type arguments it was
// named with. A trait's type parameters are ordinary generic parameters of
// every signature it declares, so a reference is what supplies the
// substitution that turns those signatures into concrete terms — at an impl
// head (`impl Into<int> for S`), at a bound (`T: Into<int>`), and inside a
// `dyn`. Interned on the arguments too, so `Into<int>` and `Into<string>` are
// two types and pointer equality still decides identity.
typedef struct {
  TraitDef *def;
  Type **type_args; // one per def->type_params, in declaration order
  int type_arg_count;
} TypeTrait;

// TY_DYN — `dyn Trait`, a trait object.
//
// Distinct from TY_TRAIT on purpose, even though both name one trait: they
// are the two halves of the same choice. A TY_TRAIT receiver (the abstract
// `Self` of a default body, or a bounded `T`) is resolved *statically* —
// codegen substitutes a concrete self and monomorphises. A TY_DYN receiver
// cannot be, so the choice travels with the value as a vtable. Sharing one
// kind would put both dispatch strategies behind one type.
//
// `assoc_types` is the trait's associated types written at the *use* site
// (`dyn Iterator<Item = int>`) — the same table an impl carries, one entry per
// `trait->assoc_types` and in that order, so the array is total and its
// ordering canonical. That is what lets interning keep deciding identity by
// pointer: `dyn Iterator<Item = int>` and `dyn Iterator<Item = string>` are
// two types. Nothing at *runtime* reads it — an associated type is erased
// exactly like a type argument — it exists so a caller can know what
// `Self.Item` is once `Self` is gone, which is what makes such a method
// object-safe at all.
typedef struct {
  Type *trait;        // TY_TRAIT — the trait, with its own type arguments
  Type **assoc_types; // one per trait def's assoc_types, in declaration order
  int assoc_type_count;
} TypeDyn;

typedef struct {
  Type *elem_type;
} TypeArray;

// A bound on one of a type parameter's *associated* types: the `I.Item: Ord`
// of `fun largest<I: Iterator>(..) where I.Item: Ord`. A plain bound constrains
// the parameter; this constrains what an impl binds the parameter's associated
// type to, which is a thing the parameter alone cannot say.
//
// It hangs off the parameter rather than off the projection because that is
// where it is *declared* and where it must be re-checked: instantiating `I`
// with `Counter` is the moment `Counter.Item: Ord` becomes a question with an
// answer. `TY_ASSOC` stays a plain (base, name) pair, and a projection is
// looked up through its base — see impl_index_implements.
//
// `equals` is the second predicate kind: `J: Iterator<Item = I.Item>` says not
// which traits the projection satisfies but *which type it is*. That makes it a
// rewrite rather than a check — see ty_assoc, which collapses `J.Item` to this
// type at construction, so the two can never be spelled apart. What is left to
// verify is that a real instantiation keeps the promise
// (check_bounds_satisfied). NULL when the parameter only carries trait bounds
// for this name; the two kinds merge into one entry, since `J: Iterator<Item =
// I.Item>` and `where J.Item: Ord` constrain the same projection.
//
// `assoc_bounds` is the same list one level down, and exists for exactly one
// spelling: `where Self.Item: Add<Output = Self.Item>` (milestone 76), a
// binding whose *subject* is a projection rather than a parameter. It is the
// only way a bounded generic can say what an operator returns, so it is what
// makes `Output` writable at a bound. Depth stops here — a nested entry is
// reached only through a trait ref's bindings, and nothing gives a third level
// a subject to name (`T.Item.Inner` is already rejected as a where-lhs).
//
// `owner` is the trait that *declares* the name, and it is half of the entry's
// key rather than a note on it: an impl is searched for the binding, and a name
// alone says neither which impl nor which entry. That was invisible while every
// associated type in the language had a unique name — until milestone 76,
// `Item` was the only one — and became load-bearing the moment all six
// `std::ops` traits declared an `Output`, since `T: Add<Output = T> +
// Mul<Output = int>` is then two entries sharing a name. See
// impl_index_assoc_type, which takes it as a filter.
typedef struct AssocBound AssocBound;
struct AssocBound {
  StringView name; // the associated type's name, e.g. "Item"
  TraitDef *owner; // the trait declaring it, e.g. Iterator (never NULL)
  Type **bounds;   // TY_TRAIT refs it must satisfy, e.g. [Ord]
  int bound_count;
  Type *equals;             // the type it is required to *be*, or NULL
  AssocBound *assoc_bounds; // bindings on *this* projection's own assoc types
  int assoc_bound_count;
};

// TY_GENERIC — an unresolved type parameter like T
typedef struct {
  StringView name; // e.g. "T"
  Type **bounds;   // TY_TRAIT refs, e.g. [Display, Into<int>]
  int bound_count;
  AssocBound *assoc_bounds; // `where T.Item: Ord`, by associated-type name
  int assoc_bound_count;
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
    TypeDyn dyn;
    TypeArray array;
    TypeGeneric generic;
    TypeAssoc assoc;
    TypeUnknown unknown;
  } as;
};

Type *ty_int(void);
Type *ty_float(void);
Type *ty_bool(void);
Type *ty_char(void);
Type *ty_string(void);
Type *ty_strbuf(void);
Type *ty_bytes(void);
Type *ty_unit(void);
Type *ty_never(void);
Type *ty_range(void);
Type *ty_poison(void);

Type *ty_unknown(uint32_t id, Type *bound, Allocator *al);
Type *ty_fun(Type **params, int param_count, Type *ret, Allocator *al);
Type *ty_tuple(Type **elems, int elem_count, Allocator *al);
Type *ty_array(Type *elem, Allocator *al);
Type *ty_generic(StringView name, Type **bounds, int bound_count,
                 AssocBound *assoc_bounds, int assoc_bound_count,
                 Allocator *al);
Type *ty_assoc(Type *base, StringView assoc_name, TraitDef *trait,
               Allocator *al);
Type *ty_struct(StructDef *def, Type **args, int argc, Allocator *al);
Type *ty_enum(EnumDef *def, Type **args, int argc, Allocator *al);
Type *ty_trait(TraitDef *def, Type **args, int argc, Allocator *al);
Type *ty_dyn(Type *trait, Type **assoc_types, int assoc_type_count,
             Allocator *al);
// the TraitDef behind a TY_DYN's trait reference.
static inline TraitDef *dyn_trait_def(const Type *t) {
  return t->as.dyn.trait->as.trait.def;
}
// the type `dyn` binds to `name`, or NULL if the trait has no such associated
// type. Total by construction, so a NULL means the name is wrong.
Type *ty_dyn_assoc(const Type *dyn, StringView name);

// ── reading a trait through its supertrait closure ───────────────────────────
//
// A trait's methods and associated types flattened over `TraitDef.flat`:
// supers first, in closure order, each trait's items in declaration order.
// This numbering *is* the `dyn` vtable and associated-type table layout, so
// building one and dispatching through it cannot disagree.
int trait_flat_method_count(const TraitDef *trait);
// `*owner_ref` (optional) receives the trait reference declaring the method,
// restated in `trait`'s own type parameters.
TraitMethodDef *trait_flat_method(const TraitDef *trait, int index,
                                  Type **owner_ref);
int trait_flat_method_index(const TraitDef *trait, StringView name);
int trait_flat_assoc_count(const TraitDef *trait);
// `*owner_def` (optional) receives the trait that *declared* the associated
// type, which is what identifies the impl binding it: a sub's flat table
// carries its supers' names, and `Item` on a `dyn DoubleEnded` is bound by the
// `impl Iterator`, not by the `impl DoubleEnded`.
StringView trait_flat_assoc_name(const TraitDef *trait, int index,
                                 TraitDef **owner_def);
int trait_flat_assoc_index(const TraitDef *trait, StringView name);

static inline bool types_equal(const Type *a, const Type *b) { return a == b; }
// does `t` mention a type parameter, an unsolved unknown or an unresolved
// projection anywhere inside it? A trait reference answers about its type
// arguments (`Into<T>` is abstract, `Into<int>` is not), which is what a bound
// has to ask before it can be checked against an impl.
bool type_is_abstract(const Type *t);
bool type_is_numeric(const Type *t);
static inline bool type_is_poison(const Type *t) {
  return t->kind == TY_POISON;
}

const char *
type_name(const Type *t); // shared underlying char buffer, not thread safe
int type_name_sprintf(const Type *t, char *buf, size_t buf_size);
int type_sprintf(const Type *t, char *buf,
                 size_t buf_size); // fully qualified, with type args

// drop the process-global interning table. Structural types are interned into
// the compiler's arena, so this must run before that arena is destroyed, and
// nothing may hold a `Type *` across it.
void type_intern_reset(void);

// ═══════════════════════════════════════════════════════════════════════════════
// SUBSTITUTION
// ═══════════════════════════════════════════════════════════════════════════════

// a mapping from type-parameter names to type arguments. Built by the checker
// while solving a call (see `infer_open_generics`), and recorded on the call
// node afterwards so codegen can monomorphise: `subst_apply` is what rewrites
// a definition's generic signature or body types into a caller's terms.
// Lives here rather than in sema.h because AST nodes store one.
struct Subst {
  StringView *params; // param names, e.g. ["T", "U", "V"]
  Type **args;        // replacement types — parallel array, same length
  int count;
};

static inline Subst subst_empty(void) { return (Subst){0}; }

// look up one parameter by name; NULL when the substitution doesn't bind it.
static inline Type *subst_find(const Subst *s, StringView name) {
  for (int i = 0; i < s->count; i++) {
    if (sv_equal(s->params[i], name)) {
      return s->args[i];
    }
  }
  return NULL;
}

// ═══════════════════════════════════════════════════════════════════════════════
// DEFINITION TABLES
// ═══════════════════════════════════════════════════════════════════════════════

typedef union {
  StringView name;
  int index;
} FieldIdent;

typedef struct {
  FieldIdent ident;
  Type *type;  // resolved field type
  bool is_pub; // visible outside the defining module (private by default)
} FieldDef;

typedef struct {
  StringView name;
  FunDef *fun;
} MethodDef;

struct StructDef {
  Module *module;
  Decl *decl; // the declaration that defines it, for the reverse walk
  bool is_pub;

  Type *self_type;

  StringView name;

  Type **type_params;   // e.g. ["T", "U"]
  int type_param_count; // if generic, else 0

  FieldDef *fields;
  int field_count;
  bool is_tuple;

  int slot; // OP_STRUCT operand; SLOT_NONE until something constructs one

  // when `field_count == 0`: the one instance every construction of this def
  // returns. See `heap_struct` — the heap owns the object, this is only where
  // it is filed.
  Obj *singleton;
};

struct VariantDef {
  StringView name;
  FieldDef *fields;
  int field_count;
  bool is_tuple;
  uint8_t tag;

  // as `StructDef.singleton`, and for the same reason: a variant with no
  // fields has no state to tell two instances apart. This is what makes
  // `Option::None` free.
  Obj *singleton;
};

struct EnumDef {
  Module *module;
  Decl *decl; // the declaration that defines it, for the reverse walk
  bool is_pub;

  Type *self_type;
  StringView name;

  Type **type_params;
  int type_param_count;

  VariantDef *variants;
  int variant_count;

  int slot; // OP_ENUM operand; SLOT_NONE until something constructs a variant
};

struct TraitMethodDef {
  StringView name;
  Type *method_type; // TY_FUNCTION with Self still unresolved
  Type **type_params;
  int type_param_count;
  int self_index; // position of `self` in method_type, -1 => assoc function
  bool has_default;

  // `where Self.Item: Ord` on the signature. This is the *obligation*: the
  // list a call site discharges against whatever concrete receiver it has,
  // since `method_type` keeps the abstract `Self` (a TY_TRAIT) that conformance
  // and call sites check against. The matching *assumption* — what the default
  // body may lean on — lives on `default_impl->type_params[0]`, the real
  // `TY_GENERIC` `Self` those same bounds are copied onto, where milestone 52's
  // machinery reads them like any other parameter's. Same fact, two readers.
  AssocBound *self_assoc_bounds;
  int self_assoc_bound_count;

  // signature is not vtable-dispatchable (no `self`, own type parameters, or
  // `Self` outside the receiver), so this method is left out of any `dyn`
  // vtable — a provided one is excluded silently, a required one makes the
  // trait non-object-safe. Set once when the signature is resolved; read by
  // codegen (which skips the slot) and the checker (which rejects a call
  // through a `dyn`). See `trait_method_undispatchable`.
  bool undispatchable;

  // the default body as a definition of its own: a generic function whose
  // first type parameter is `Self`, bounded by this trait. NULL if required.
  // `method_type` keeps the abstract `Self` (a TY_TRAIT) — it is what impl
  // conformance and call sites check against; only the *body* needs a `Self`
  // a substitution can bind, so it is compiled once per concrete receiver.
  FunDef *default_impl;
};

typedef struct {
  StringView name;
  Type *type;
} AssocTypeDef;

// One trait in another's *supertrait closure* — the trait itself, plus the
// reference naming it, restated in the terms of the trait whose closure this
// is (`trait Pair<T>: Into<T>` puts `Into<T>` here with the sub's own `T`).
typedef struct {
  TraitDef *def;
  Type *ref; // TY_TRAIT
} TraitFlat;

struct TraitDef {
  Module *module;
  bool is_pub;

  StringView name;

  Type *self_type;

  Type **type_params;
  int type_param_count;

  // Parallel to `type_params`: the type a reference that omits an argument
  // gets, or NULL where the argument is required (milestone 75). Kept as
  // unresolved AST, because `Self` in a default means *the type the trait is
  // being applied to* — the impl's self type at an impl head, the bounded
  // parameter at a bound — so there is no one type to resolve it to once.
  TypeNode **type_param_defaults;

  // `trait DoubleEnded: Iterator` — the *direct* supers, as written. The
  // obligation an `impl` of this trait discharges (see
  // tc_check_impl_conformance); what a *bound* on it offers is `flat`.
  Type **supers;
  int super_count;

  // The transitive closure of `supers`, deduped, supers-first and this trait
  // last. It is the answer to every question a supertrait changes — which
  // traits a `T: This` bound implies, which methods and associated types that
  // receiver may name, and the order a `dyn This` vtable is laid out in — so
  // those four readers share one list rather than four walks. A trait with no
  // supers has exactly one entry (itself), which is what lets each reader drop
  // its special case rather than gain one. The declare pass installs that
  // one-entry answer so the list is never absent; trait_build_closure replaces
  // it once the supers are resolved, and `flat_built` is its memo (a cycle
  // reaches a trait already on the stack, and must stop rather than recurse).
  TraitFlat *flat;
  int flat_count;
  bool flat_built;

  TraitMethodDef *methods;
  int method_count;

  AssocTypeDef *assoc_types;
  int assoc_type_count;
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
};

typedef struct {
  StringView name;
  Type *param_type;
  bool is_self; // implicit "self"
} ParamDef;

// `@native("io_print")` / `@intrinsic("array_len")` on a bodyless `fun` — the
// attribute *is* the body. `@lang("display")` is the odd one out: a *marker*,
// not a body, so it sits on a normally-bodied `trait`/`enum`/`fun` and tells
// the compiler to track that definition as a lang item (see
// `tc_register_lang_*`). A definition may carry a body attribute and a `@lang`
// at once — `float` is both — which is why the two live in separate fields
// (`DeclFun.attr` for the body, `Decl.lang_attr` for the marker) rather than
// one slot.
typedef enum {
  ATTR_NONE = 0,
  ATTR_NATIVE,    // a C function, called through the ordinary OP_CALL
  ATTR_INTRINSIC, // an opcode the call lowers to inline
  ATTR_LANG,      // a marker: this definition is a std lang item
  ATTR_ALLOW,     // policy: silence one lint over this definition
} AttrKind;

typedef struct {
  AttrKind kind;
  StringView name; // the registry key inside the parentheses
  Span span;
} AttrNode;

struct FunDef {
  Module *module;
  bool is_pub;

  StringView name;
  bool is_closure;
  Type *fun_type;

  Type **type_params;
  int type_param_count;

  // the impl this method belongs to; NULL for top-level functions and
  // closures. Its type params are in scope in the body just like the
  // method's own, so both together are what an instantiation must bind.
  ImplDef *impl;

  // the body codegen compiles, and the span to report against. NULL/zero for
  // a native or intrinsic (whose body is in C) and for definitions decoded
  // from a bytecode image (which carries chunks, not ASTs). A generic
  // definition is compiled once per instantiation, so codegen needs to reach
  // the body from the FunDef alone — a call site in another module has
  // nothing else.
  Expr *body;
  Span span;

  ParamDef *params;
  int param_count;
  Type *return_type;

  // where the body actually is. `chunk` for a function written in ducktape
  // (filled by codegen, NULL until then); otherwise `native_kind` says which
  // of the two C tiers this is. `native_name` is the registry key, kept
  // because a bytecode image writes a native by *name* — a C function pointer
  // cannot be serialised, so `bc_load` re-binds it against the running binary.
  struct Chunk *chunk;
  AttrKind native_kind;
  NativeFn native;      // ATTR_NATIVE: the C function OP_CALL invokes
  uint8_t intrinsic_op; // ATTR_INTRINSIC: the OpCode a call lowers to
  StringView native_name;

  // SLOT_NONE until codegen *reaches* this definition — a slot is what a
  // reference costs, not what a declaration costs. A generic never takes one
  // at all: its instances do.
  int slot;

  // What the inliner made of this body: AST nodes when it may be spliced into
  // a caller, -1 when it may not, 0 before anything asked. Memoised here
  // because the answer is a property of the body and every call site asks it.
  int inline_cost;
};

// a function whose body is in C rather than in ducktape. `!` monomorphised
// (the runtime is uniform in type arguments, so one C body serves every `T`)
// and never compiled to a chunk.
static inline bool fun_is_native(const FunDef *f) {
  return f->native_kind != ATTR_NONE;
}

// "no slot handed out": the initial state of every `slot` field above, and
// permanently the state of anything nothing reaches.
#define SLOT_NONE (-1)

// a definition whose body can only be compiled once its type parameters — its
// own and any its impl introduces — are bound to concrete types.
static inline bool fun_is_generic(const FunDef *f) {
  return f->type_param_count > 0 ||
         (f->impl != NULL && f->impl->type_param_count > 0);
}

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
  TYNODE_NEVER, // !
  TYNODE_NAMED, // int, MyStruct, Option<T>
  TYNODE_TUPLE, // (A, B)
  TYNODE_ARRAY, // [T]
  TYNODE_FUN,   // fun(A, B): R
  TYNODE_SELF,  // Self
  TYNODE_ASSOC, // T.Item
  TYNODE_DYN,   // dyn Trait
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

// `Item = int` inside `dyn Iterator<Item = int>`.
typedef struct {
  StringView name;
  TypeNode *type;
  Span span;
} AssocBindingNode;

// TYNODE_DYN — the path names the trait; the bracket list after it carries
// two different things, which is why it is parsed here rather than by
// `parse_path`. `<int>` is a type argument, supplying one of the trait's own
// type parameters; `<Item = int>` is a binding, saying what `Self.Item` means
// once `Self` is erased. Arguments come first, exactly as in the source:
// `dyn Into<int>`, `dyn Iterator<Item = int>`, or both.
typedef struct {
  Path path;
  TypeNode **type_args;
  int type_arg_count;
  AssocBindingNode *bindings;
  int binding_count;
} TypeNodeDyn;

struct TypeNode {
  TypeNodeKind kind;
  Span span;
  Type *resolved; // filled in by type checker; NULL util then

  union {
    TypeNodeNamed named;
    TypeNodeDyn dyn;
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
  // the binding a *shorthand* introduced, under the field's own name. A field
  // with a sub-pattern binds nothing itself, and leaves this NULL.
  VarEntry *entry;
} FieldPat;

// Pattern union variants
typedef struct {
  StringView name;
  VarEntry *entry; // the binding this introduced; NULL until the checker runs
} PatternBind;

typedef struct {
  Path path;
  FieldPat *fields;
  int field_count;
  VariantDef *resolved_variant; // NULL until resolver runs
} PatternVariant;

typedef struct {
  Path path;
  FieldPat *fields;
  int field_count;
  StructDef *resolved_struct; // NULL until resolver runs
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

// A single trait in a bound, optionally generic: Clone, Iterator<int>, From<T>
//
// The `<..>` after the name carries the same two lists a `dyn Trait<..>` does,
// and is parsed by the same function: the trait's own type arguments (which go
// into the path's last segment, since they are what makes the *trait
// reference*) and its associated-type bindings, `Iterator<Item = I.Item>`,
// which say nothing about the trait and everything about the type being bounded
// — so they are lifted onto that parameter's `AssocBound` list instead.
typedef struct {
  Path path; // the trait name, possibly qualified: std::fmt::Display
  AssocBindingNode *bindings;
  int binding_count;
  Span span;
} TraitRef;

// One or more trait refs joined by +: Clone + Iterator<int> + From<T>
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

// The whole where clause: where T: Clone, U: Into<string>
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
  EXPR_CHAR,   // 'a'
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
  EXPR_CALL,        // f(a, b)
  EXPR_INDEX,       // arr[i]
  EXPR_FIELD,       // obj.field
  EXPR_METHOD_CALL, // obj.method(args)
  EXPR_CAST,        // expr as Type
  EXPR_PROPAGATE,   // expr?              — Result propagation

  // ── Structural (pass 2+) ──────────────────────────────
  EXPR_BLOCK, // { stmts... expr? }
  EXPR_IF,    // if cond { } else { }
  EXPR_WHILE, // while cond { }
  EXPR_LOOP,  // loop { }
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
  EXPR_CLOSURE,     // fun(x: int): int { x + 1 }

  // ── string interpolation segment (pass 3 QoL) ─────────
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
  VarEntry *entry; // the binding it introduced; NULL until the checker runs
} ClosureParam;

typedef struct {
  StringView name;
  TraitBound
      inline_bound; // refs == NULL / ref_count == 0 means no inline bound
  // `<Rhs = Self>` — the type a reference that omits this argument gets
  // instead. NULL means the argument is required. Traits only (milestone 75):
  // a default is what lets `Add` gain an `Rhs` parameter without every
  // existing `T: Add` and `impl Add for int` having to name it.
  TypeNode *default_type;
  Span span;
} TypeParamNode;

// A format spec written after a `:` in an interpolation segment — the `>8` in
// `{v:>8}`, the `.3` in `{f:.3}`, or both fused in `{f:>8.3}`. This is the
// whole grammar of a spec, and it is pure front-end sugar (milestone 35): the
// checker desugars it into calls to `std::string`'s `pad_*` and
// `std::fmt::float`, so nothing downstream of `check_interpol_seg` ever sees a
// `FormatSpec` — codegen, the VM and the image format are untouched.
typedef enum {
  FMT_ALIGN_NONE,   // no width/alignment
  FMT_ALIGN_START,  // `>` — value at the right, fill on the left (pad_start)
  FMT_ALIGN_END,    // `<` — value at the left, fill on the right (pad_end)
  FMT_ALIGN_CENTER, // `^` — value centred (pad_center)
} FormatAlign;

typedef struct {
  bool present;       // was there a `:` spec on this segment at all
  FormatAlign align;  // FMT_ALIGN_NONE unless a width was given
  int64_t width;      // field width, meaningful when align != FMT_ALIGN_NONE
  uint32_t fill;      // fill codepoint; ' ' unless a `'c'` literal overrode it
  bool has_precision; // was a `.N` precision given
  int64_t precision;  // decimal places, meaningful when has_precision
} FormatSpec;

typedef enum { ISEG_TEXT, ISEG_EXPR } InterpolSegKind;
typedef struct {
  InterpolSegKind kind;
  StringView text; // (slice into source for ISEG_TEXT)
  Expr *expr;      // ISEG_EXPR
  FormatSpec spec; // ISEG_EXPR; `.present` is false for a bare `{v}`
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
  // the function this path names, cached for codegen (which emits its global
  // slot). Set both by resolve_callee, for a multi-segment path, and by
  // EXPR_PATH resolution, for a single-segment one. NULL until resolved, and
  // for a name bound to a local of function type — a closure or parameter
  // lives in a stack slot, not a global one.
  FunDef *resolved_fun;

  // when `resolved_fun` is generic: the type arguments this path instantiates
  // it with, covering both its own type params and its impl's. Entries may
  // still name the *enclosing* definition's type params (`f::<T>()` inside a
  // generic `f`), which the monomorphiser substitutes away.
  Subst inst;

  // set instead of `resolved_fun` when the path is qualified by a type
  // parameter (`T::from(v)`, or `Self::from(v)` in a default body): the trait
  // signature is all the checker knows, so which impl supplies the body waits
  // until `bound_self` is substituted — exactly ExprMethodCall.bound_self one
  // spelling over, for the case where there is no receiver to carry it.
  Type *bound_trait; // TY_TRAIT — the trait reference the bound named
  Type *bound_self;

  // the binding a single-segment path resolved to, for a name that is one.
  // NULL for a path naming an item — a function, a variant, an import.
  VarEntry *resolved_local;
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
  // A compound `a op= b` whose operator is a trait call rather than an opcode
  // (`v += w` on a type with `impl Add`). The checker resolves `a.add(b)` and
  // parks the call here instead of rewriting this node, because the place has
  // to be compiled once and the call is only half of what happens to it — so
  // codegen reads the receiver off the stack and never compiles `object`.
  // NULL for `=` and for every built-in operand type.
  Expr *op_call;
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

  // set instead of `resolved_method` when the receiver's impl omitted the
  // method and inherited the trait's default body: that body is its own
  // definition, generic over `Self` (see TraitMethodDef.default_impl).
  FunDef *resolved_default;

  // the type arguments this call instantiates the target with (impl params
  // then the method's own), in the enclosing definition's terms — see
  // ExprPath.inst. A call routed through a trait (a bound, or an inherited
  // default) also binds `Self` to the receiver's type, since that is the
  // default body's own first type parameter.
  Subst inst;

  // set instead of `resolved_method` when the receiver is abstract and the
  // call went through a trait bound: which impl provides the body is not
  // knowable until `bound_self` is substituted with a concrete type, so
  // codegen redoes the impl lookup per instantiation.
  Type *bound_trait; // TY_TRAIT — the trait reference the bound named
  Type *bound_self;
} ExprMethodCall;

typedef struct {
  Expr *operand;
  TypeNode *target_type;

  // `as?` — the fallible form: a downcast from a trait object back to one
  // concrete type, evaluating to an `Option<Target>`. Plain `as` is a total
  // conversion between numbers and cannot fail, so the two share only a
  // keyword. Everything below is filled by the checker for the fallible form
  // and stays NULL for the total one.
  bool fallible;

  // the trait *reference* the operand's `dyn` was written with. This is the
  // half of the vtable memo key the target type does not supply, so codegen
  // asks for the very table a coercion of `target` would have built and
  // compares it with the one the value carries — see `compile_downcast`.
  Type *dyn_trait;
  Type *target; // the resolved target, i.e. the vtable key's self type

  // the `Option` the result is built from: the enum lang item, and the two
  // variants by name. Recorded here for the same reason `ExprFor` records
  // them — codegen constructs the answer and cannot look a lang item up.
  EnumDef *option_enum;
  VariantDef *some_variant;
  VariantDef *none_variant;
} ExprCast;

typedef struct {
  Expr *operand;
  VariantDef *ok_variant;  // the Result-like enum's single-field Ok(T)
  VariantDef *err_variant; // ...and Err(E); both NULL until resolved
} ExprPropagate;

// A loop's or labelled block's name, or `name.len == 0` when it has none.
// Every loop form carries one, a block may, and every `break`/`continue` may
// name one — so the pair travels together rather than being two fields repeated
// six times. The name keeps its leading quote, so a diagnostic prints it
// exactly as it was written.
typedef struct {
  StringView name;
  Span span;
} LoopLabel;

// A label makes the block a `break` target (`'a: { .. break 'a v; .. }`) and
// changes nothing else about it — which is why it is a field here rather than a
// node of its own. The exits then number two kinds instead of one: every
// `break` naming it, and running off the end with the tail. Both carry a value,
// so the block's type is the join of all of them.
typedef struct {
  LoopLabel label; // absent name = an ordinary block, which nothing can break
  Stmt **stmts;
  int stmt_count;
  Expr *tail_expr;
} ExprBlock;

// `binding` is what makes this an `if var P = subject` rather than a plain
// `if`: when it is set, `condition` is the *subject* — a value of any type,
// not a bool — and the test is whether it has the pattern's shape.
typedef struct {
  Expr *condition;
  Pattern *binding; // NULL for a plain `if`
  Expr *then_block;
  Expr *else_branch;
} ExprIf;

typedef struct {
  LoopLabel label;
  StringView var_name;
  Span var_span;
  VarEntry *var_entry; // the loop variable's binding; NULL until checked
  Expr *iterable;
  Expr *body;

  // set only when the iterable is a user type implementing `Iterator` (neither
  // an array nor a range): the synthesised `iterable.next()` call the loop
  // drives, and the two variants of the `Option` its result is taken apart by.
  // NULL/absent for an array or range, which codegen compiles by index instead.
  Expr *next_call;
  VariantDef *some_variant; // `Some(T)` — its field 0 is the bound value
  VariantDef *none_variant; // `None` — its tag ends the loop
} ExprFor;

// same reading of `condition` as `ExprIf`: with a `binding` the loop runs for
// as long as the re-evaluated subject keeps matching.
typedef struct {
  LoopLabel label;
  Expr *condition;
  Pattern *binding; // NULL for a plain `while`
  Expr *body;
} ExprWhile;

// `loop { .. }`. No condition is not a `while` with one field missing: a
// `while` always has an exit and so always types `()`, while this one leaves
// only through a `break` — which is why a `loop` alone has a value to give its
// breaks, and why with none that leaves it types `!`. Both answers are the
// join the checker accumulates on its `CheckLoop` frame, so the node itself
// records nothing.
typedef struct {
  LoopLabel label;
  Expr *body;
} ExprLoop;

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

  // set by the checker when this expression's value flows into a `dyn Trait`
  // and so must be wrapped with a vtable on the way (see check_coerce_dyn).
  // NULL for the overwhelming majority of nodes. Recorded here rather than
  // applied to `resolved_type` for the same reason `inst` is: at the moment
  // the coercion is discovered the receiver type may still be an unsolved
  // unknown, and codegen is the consumer either way.
  //
  // The *trait reference* (a TY_TRAIT, with its type arguments), not the trait
  // definition: `Into<int>` and `Into<string>` are two vtables over one type.
  Type *coerce_dyn;

  union {
    int64_t int_val;
    double float_val;
    bool bool_val;
    uint32_t char_val;
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

    ExprCast cast;

    ExprPropagate propagate;

    ExprBlock block;

    ExprIf if_expr;

    ExprFor for_expr;

    ExprWhile while_expr;

    ExprLoop loop_expr;

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
  STMT_BREAK,    // break expr? ;
  STMT_CONTINUE, // continue ;
  STMT_POISON,   // sentinel
} StmtKind;

// Stmt union variants
typedef struct {
  Expr *expr;
} StmtExpr;

typedef struct {
  // the same Pattern a match arm uses, restricted to the irrefutable ones by
  // the checker: `var x`, `var (a, b)`, `var Point { x, y }`, nested freely.
  Pattern *binding;
  TypeNode *type_annotation; // NULL if inferred
  Expr *initializer;
  // `else` inverts the restriction above: the pattern must be refutable, and
  // this block is where it did not match. It has to diverge, since the names
  // the binding introduces do not exist below it.
  Expr *else_block; // NULL for a plain `var`
} StmtVar;

typedef struct {
  Expr *value; // NULL for bare "return;"
} StmtReturn;

// `break 'label? expr?;`. A value goes to a `loop` or a labelled block, the two
// targets whose other exit has one to agree with — a `while` or `for` also
// leaves by finishing, and that exit carries nothing. Unlabelled it names the
// innermost *loop*, skipping any block between: labelling a block must not take
// a bare `break` away from the loop around it. Labelled it names whatever
// declared the name, however many targets it has to leave to get there.
typedef struct {
  LoopLabel label; // absent name = the innermost loop, never a block
  Expr *value;     // NULL for bare "break;"
} StmtBreak;

typedef struct {
  LoopLabel label;
} StmtContinue;

struct Stmt {
  StmtKind kind;
  Span span;

  union {
    StmtExpr expr_stmt;
    StmtVar var_stmt;
    StmtReturn return_stmt;
    StmtBreak break_stmt;
    StmtContinue continue_stmt;
  } as;
};

// ═══════════════════════════════════════════════════════════════════════════════
// DECLARATIONS
// ═══════════════════════════════════════════════════════════════════════════════

typedef enum {
  DECL_USE,
  DECL_MOD,
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
  Span span;        // the item as written, for per-item import diagnostics
  // set wherever the name this alias binds is looked up, in any of the three
  // namespaces an import can reach. Read once, after the module is checked,
  // by the unused-import warning.
  bool used;
} UseAlias;

typedef struct {
  UseAlias *aliases;
  int count;
  // `use a::E::*;` — every variant of the qualifier enum, under its own name.
  // The names are unknown until the enum is resolved, so `aliases` is empty and
  // `span` carries the `*` for the diagnostics the expansion may emit.
  bool is_glob;
  // a glob writes no alias, so the whole target is the unit of use: any one of
  // the names it expands to counts for all of them. The `used` beside each
  // alias is the same flag one namespace finer.
  bool used;
  Span span;
} UseTarget;

typedef struct {
  union {
    StringView name;
    int index; // for tuple structs
  } ident;
  TypeNode *type_annotation;
  bool is_pub; // `pub` prefix: visible outside the defining module
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
  TypeNode *return_type;     // NULL -> unit
  WhereClause *where_clause; // NULL if no where clause
  Expr *default_body;        // NULL -> required

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
  // `path` is the module prefix for a braced import (`use a::b::{X, Y}`), but
  // the *whole* written path for a bare one (`use a::b;`) — because a bare use
  // is either an item import (b is an item of module a) or a module import (b
  // is module a::b bound as a qualifier). `bare` records which shape the parser
  // saw; `mod_collect_imports` walks the declared tree and sets
  // `is_module_import`.
  Path path;
  UseTarget target;
  // the enum the imported names are variants of (`use a::E::V;` → `E`), empty
  // when they are items of the module. Set at collect, by the same file-
  // existence question: one more segment sits between the module and the names.
  StringView qualifier;
  bool bare;             // no `{...}` list: path includes the trailing name
  bool is_module_import; // binds a module qualifier, not items (set at collect)
  bool is_scope_import;  // the path's head names no declared module, so the
                         // qualifier is a name in *this* module's type scope —
                         // its own enum, an imported one, or a preluded one.
                         // Resolved after this module resolves (set at collect)
  bool from_prelude;     // synthesised by mod_inject_prelude, not written in
                         // source: yields silently on a name collision so a
                         // local decl or explicit import always wins.
} DeclUse;

// `mod x;` / `pub mod x;` — `x` is a child module of this one: part of the
// build unit, and a segment any absolute path may walk through. It binds `x`
// alongside the module's items (a name is a child or an item, never both),
// which is what lets path resolution walk greedily with no lookahead.
typedef struct {
  StringView name;
  Span name_span; // the identifier alone, for the diagnostics privacy points at
} DeclMod;

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
  TraitBound supers; // `trait A: B + C` — ref_count 0 when none was written
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
  Expr *body;                // EXPR_BLOCK or shorthand; NULL when `attr` binds
                             // the body to C instead
  AttrNode attr;
  // resolved:
  FunDef *def;
} DeclFun;

typedef struct {
  Pattern *binding;
  TypeNode *type_annotation;
  Expr *initializer;
} DeclVar;

struct Decl {
  DeclKind kind;
  bool is_pub;
  // `@lang("…")` if this decl is marked as a lang item, else `.kind ==
  // ATTR_NONE`. Lives on the shared Decl because a marker applies uniformly to
  // a trait, enum, or fun — unlike a body attribute, which is fun-only and
  // stays on `DeclFun.attr`.
  AttrNode lang_attr;
  // the lints `@allow("…")` silences over this declaration and everything
  // inside it, as `LINT_BIT` flags. A mask rather than a list because the set
  // is closed and small, and because nesting is then a union: see
  // `diag_push_allowed`. Resolved in the parser — a lint name is a key into a
  // table the compiler already owns, so there is nothing for a later phase to
  // look up.
  unsigned allow_mask;
  // reached from a root of the item-use walk (`unused_item`). A mark on the
  // declaration rather than a side table, so an edge is one pointer read.
  bool item_live;
  Span span;
  // just the name a `fun`/`struct`/`enum`/`trait` introduces — what a
  // diagnostic *about the item* points at, where `span` covers the whole
  // declaration. Zero for every other kind, none of which introduces one.
  Span name_span;
  union {
    DeclUse use_decl;
    DeclMod mod_decl;
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

// The binary operator an `ExprAssign.op` is spelled with: `+=` means `+`, and
// that is the whole of the language rule — `a op= b` is `a = a op b`. It lives
// out here because the checker and codegen both read that sentence, and a
// second copy of the map is a second chance for them to disagree about it.
// TOKEN_ERROR for anything that is not a compound assignment, TOKEN_EQ (a
// plain one, which has no operator) included.
TokenType token_compound_binary_op(TokenType op);

// ═══════════════════════════════════════════════════════════════════════════════
// DEBUG
// ═══════════════════════════════════════════════════════════════════════════════

void dump_program(const Program *p, int indent);
void dump_decl(const Decl *d, int indent);
void dump_expr(const Expr *e, int indent);
void dump_stmt(const Stmt *s, int indent);
void dump_pattern(const Pattern *p, int indent);
void dump_type(const Type *t);
