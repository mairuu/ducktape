#pragma once

#include "allocator.h"
#include "ast.h"
#include "diag.h"

typedef struct Module Module;
typedef struct ModuleRegistry ModuleRegistry;
typedef struct InferCtx InferCtx;
typedef struct TypeChecker TypeChecker;
typedef struct ValueScope ValueScope;
typedef struct TypeScope TypeScope;
typedef struct ImplIndex ImplIndex;
typedef struct TypeResolver TypeResolver;
typedef struct ResolveCtx ResolveCtx;
typedef struct CheckCtx CheckCtx;

// ═══════════════════════════════════════════════════════════════════════════════
// Substitution  (`struct Subst` itself lives in ast.h — AST nodes store one)
// ═══════════════════════════════════════════════════════════════════════════════

// build a substitution from two parallel arrays of equal length.
void subst_init(Subst *s, StringView *params, Type **args, int count);

// recursively replace TY_GENERIC nodes whose .name matches an entry.
// unmatched generics pass through unchanged.
Type *subst_apply(const Subst *s, Type *t, Allocator *al);

// collapse every `T.Assoc` projection in `t` whose base has become a real type,
// reading the binding off the impl that applies — `Counter.Item` becomes `Int`.
// A projection over a type parameter or a trait's abstract `Self` has no answer
// yet and is left standing.
//
// This is what infer_apply does for the checker, minus the inference: codegen
// has no InferCtx but does have an impl index, and after substituting a call's
// type arguments it needs `I.Item` to become the element type before it can key
// an instantiation on it.
Type *assoc_apply(ImplIndex *impls, Type *t, Allocator *al);

// instantiate a generic item: for each TY_GENERIC in `type_params`, create
// a fresh TY_UNKNOWN or use the provided type argument (in case of explicit
// type arguments) and build a Subst. after unification, apply infer_apply to
// the substituted return type to read out the inferred type arguments.
//
// Each array carries its own length, and `type_arg_count` is the one that
// matters: a call with no turbofish supplies *zero* type arguments against a
// definition with several type parameters, so the two counts are routinely
// different and `type_args` may only be indexed within its own.
Subst infer_open_generics(InferCtx *ctx, Type **type_params, int param_count,
                          Type **type_args /* nullable */, int type_arg_count,
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
// are fully resolved. no-op if `impl` is already present, which is what makes
// unioning two dependency sets idempotent for a diamond import.
void impl_index_add(ImplIndex *idx, ImplDef *impl);

// would `a` and `b` both apply to some receiver, for the same trait? Two such
// impls in one visible set make method selection order-dependent, which is
// exactly the silent breakage module-granular impls exist to prevent.
// Inherent impls never conflict: splitting a type's methods across several
// `impl` blocks is ordinary.
bool impl_defs_conflict(ImplDef *a, ImplDef *b, Allocator *al);

// Do `a` and `b` give one receiver the same *inherent* method name? Splitting
// a type's methods across impl blocks is ordinary, so the granularity here is
// the name rather than the impl head: two blocks may share a type, but not a
// name. On a hit *out_name is the first name they both spend.
//
// "Inherent" means reached by the receiver alone — an impl heading no trait,
// and also the extra methods a trait impl is allowed to carry. A method the
// trait *declares* is excluded: a bound or a trait-qualified path names which
// one was meant, which is why two traits may both declare `next`.
bool impl_defs_share_method(ImplDef *a, ImplDef *b, StringView *out_name,
                            Allocator *al);

// Does an inherent method of `impl` spend a name the enum it is written for has
// already spent on a variant? `Enum::name` resolves the variant first, so the
// method would be unreachable — the same loss as two impls sharing a name, and
// the same "inherent" granularity. False for any non-enum self type. On a hit
// *out_name is the first name spent twice.
bool impl_shadows_enum_variant(ImplDef *impl, StringView *out_name);

// find a method named `name` applicable to `self_type`, trying every
// registered impl. on a match, *out_match is filled with the impl and the
// substitution mapping the impl's type params to the concrete type args
// inferred from `self_type` (empty subst if the impl isn't generic).
// find a method for `self_type`. when `bare_path` is set (the caller is
// resolving a path like `Point::new`, not a method receiver), `self_type` is a
// generic type's canonical self, and `infer` is non-NULL, the impl is selected
// by method name instead and its type params are opened as fresh unknowns for
// the call site to solve. Receivers must pass bare_path=false: TY_GENERIC is
// interned, so a generic impl's `Self` is pointer-identical to the struct's
// canonical self and the type alone can't distinguish the two cases.
// `trait_ref` narrows the search to impls of that trait reference (a TY_TRAIT,
// with its type arguments), or NULL for "any trait" — which is what a receiver
// wants, since a method may come from any impl. Codegen passes one when the
// call was resolved through a bound: `Into<Int>` and `Into<String>` are two
// impls for one type, and only the bound says which body was meant.
MethodDef *impl_index_method(ImplIndex *idx, Type *self_type, Type *trait_ref,
                             StringView name, Type *ret_hint,
                             ImplMatch *out_match, InferCtx *infer,
                             bool bare_path, Span span, Allocator *al);

// find a trait method `self_type` inherits by default: an applicable
// `impl Trait for <self_type>` exists and the trait declares `name` with a
// default body the impl didn't override. call after impl_index_method misses.
// on a match *out_impl/*out_trait/*out_subst describe the impl it comes
// through, enough to project the trait signature into concrete terms.
TraitMethodDef *impl_index_default_method(ImplIndex *idx, Type *self_type,
                                          Type *trait_ref, StringView name,
                                          ImplDef **out_impl,
                                          TraitDef **out_trait,
                                          Subst *out_subst, Allocator *al);

// does `type` implement the trait reference `trait_ref` (a TY_TRAIT, with its
// type arguments)? true if some registered impl heads `impl [<..>] trait for T`
// matching both the self type and the reference.
bool impl_index_implements(ImplIndex *idx, Type *type, Type *trait_ref,
                           Allocator *al);

// ═══════════════════════════════════════════════════════════════════════════════
// Inference
// ═══════════════════════════════════════════════════════════════════════════════

// a bounded type parameter instantiated with an explicitly written type
// argument (`need_a::<Q>(..)`). No unknown is ever created for it, so
// infer_check_bounds has nothing to inspect after solving — the pair is
// recorded here instead and checked alongside the solved unknowns.
typedef struct {
  Type *param; // the source TY_GENERIC, carrying the bounds
  Type *arg;   // the type argument written at the call site
  Span span;
} ExplicitBound;

struct InferCtx {
  Type **solutions; // NULL=free, TY_UNKNOWN=redirect, else solved
  Type **nodes;     // the original TY_UNKNOWN node (never changes)
  int cap;          // allocated slot count
  uint32_t next_id; // next fresh id

  ExplicitBound *explicit_bounds;
  int explicit_bound_count, explicit_bound_cap;

  // needed to collapse a `T.Assoc` projection once T is solved: the binding
  // lives on whichever impl applies. see infer_apply.
  ImplIndex *impls;

  Allocator *al;
};

void infer_init(InferCtx *ctx, ImplIndex *impls, Allocator *al);

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

// After infer_finalize: ensure every solved type parameter that carried trait
// bounds was instantiated with a type implementing them.
// `tc` is only consulted to explain a failure: the bound is checked against
// ctx->impls, the enclosing module's visible set.
void infer_check_bounds(InferCtx *ctx, TypeChecker *tc, DiagBag *diags,
                        Allocator *al);

// ═══════════════════════════════════════════════════════════════════════════════
// TypeChecker
// ═══════════════════════════════════════════════════════════════════════════════

// The arithmetic operator traits of `std::ops`, in one enum because the
// compiler treats them as one mechanism: an operator token selects a slot, and
// everything after that (the lang-item name, the trait's name in a diagnostic,
// the method the rewrite calls) is a row of `ops_trait_table` in sema.c. `Neg`
// is the unary one and sits last for that reason.
typedef enum {
  OPS_ADD,
  OPS_SUB,
  OPS_MUL,
  OPS_DIV,
  OPS_REM,
  OPS_NEG,
  OPS_TRAIT_COUNT,
} OpsTrait;

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

  // Which impls a module may *select* from is Module.visible_impls; there is
  // deliberately no program-wide index to select from. This list is every
  // impl in the program and exists for one purpose: so a failed lookup can
  // say "an implementation exists in module X, but this module does not
  // import it" instead of "no method named 'show'". Diagnostics only —
  // nothing here is ever chosen.
  ImplIndex all_impls;

  Type *t_int, *t_float, *t_bool, *t_char, *t_string, *t_strbuf, *t_range,
      *t_unit, *t_never, *t_poison;

  // Lang items: std names the *compiler* resolves for a construct that never
  // spells them — `"{v}"`, `a < b`, a `{v:>8}` spec. Each is captured when its
  // module registers. Since milestone 45 the prelude imports every one of
  // these modules into every user program (`mod_inject_prelude`), so in
  // practice they are always populated; the NULL guards below survive only as
  // defensive branches (a std module that fails to load), no longer the
  // user-facing "you forgot to import it" path the prelude retired.
  //
  // `std::fmt`'s `Display`: interpolation of a non-primitive is a call to
  // `v.to_string()`, and *which* trait declares that method is the language's
  // decision, not whatever is in scope.
  TraitDef *display_trait;

  // `std::cmp`'s `Ord`, captured the same way and for the same reason as
  // `display_trait`: a non-numeric `<`/`<=`/`>`/`>=` desugars to `a.cmp(b) <
  // 0`, and *which* trait declares `cmp` is the language's decision. Numeric
  // comparison stays a built-in opcode, so only a non-primitive operand
  // reaches for this. See `rewrite_ord_comparison`.
  TraitDef *ord_trait;

  // `std::iter`'s `Iterator`, captured the same way: `for x in it` over a value
  // that is neither an array nor a range requires `it` to implement this trait,
  // and drives `it.next()` — a name the loop never spells, so the language must
  // own it. Preluded, so it is populated in every program. See the `EXPR_FOR`
  // case in `resolve_expr`.
  TraitDef *iterator_trait;

  // `std::ops`'s six operator traits, captured the same way and for the same
  // reason: `a + b` on a non-numeric operand desugars to `a.add(b)`, and which
  // trait declares `add` is the language's decision. Indexed by `OpsTrait` so
  // one array and one table serve all six — they differ only in the token that
  // reaches for them and the method they name. Numeric arithmetic stays a
  // built-in opcode, so only a non-primitive operand looks here. See
  // `rewrite_ops_binary` / `rewrite_ops_unary`.
  TraitDef *ops_traits[OPS_TRAIT_COUNT];

  // `std::option`'s `Option`, and the first lang item that is a *type* rather
  // than something to call. Every other consumer of `Option` takes one apart
  // and so needs no handle on the enum at all — `for` unwraps `next()`'s
  // result structurally, and `?` builds its early `Err` out of the operand's
  // own definition. A downcast (`d as? T`) is the first construct that must
  // *produce* an `Option` with no operand to borrow the definition from, so it
  // is the first that has to name the type. Preluded, like the rest.
  EnumDef *option_enum;

  // The format functions a `{v:>8}` / `{f:.3}` spec desugars to, captured the
  // same way `display_trait` is and for the same reason: the user never types
  // these names. `pad_*` live in `std::string`, `fmt_float` in `std::fmt` —
  // both preluded. See `check_interpol_seg`.
  FunDef *fmt_pad_start, *fmt_pad_end, *fmt_pad_center, *fmt_float;

  DiagBag *diags;
  Allocator *al;
};

void tc_init(TypeChecker *tc, DiagBag *diags, Allocator *al);

void tc_destroy(TypeChecker *tc);

void tc_register_module(TypeChecker *tc, Module *m);

// inject every `use`d item into m's value and type scopes under its alias.
//
// must run in topological order, immediately before tc_resolve_module(m):
// a dependency's exported names only acquire types during *its* resolve, and
// m's own signatures resolve against the imports this puts in place. That is
// why linking is not a standalone pass.
void tc_link_imports(TypeChecker *tc, Module *m, ModuleRegistry *reg);

// fill in every variant import whose qualifier is a name rather than a file
// (`use Event::A;`, `use Color::Red;`, `use Option::Some;`) and expand every
// glob (`use E::*;`).
//
// runs immediately *after* tc_resolve_module(m), not before it with the other
// linking: the qualifier is resolved against m's own type scope, which is
// complete only then, and a glob needs the enum's variants filled in. Nothing
// is lost by the wait — a variant is never part of a signature, so only bodies,
// checked a phase later, can name one. `tc_link_imports` has already claimed
// each name in source order, so collisions are reported there, not here.
void tc_link_scope_imports(TypeChecker *tc, Module *m, ModuleRegistry *reg);

// seed m->visible_impls with every impl reachable through its `use`
// declarations, and report any pair that conflicts.
//
// runs in topological order alongside tc_link_imports, and for the same
// reason: a dependency's own impls are only registered as *it* is resolved.
// m's own impls are appended later, by resolution, as each `impl` block is
// reached — so the union is complete only once tc_resolve_module(m) returns.
void tc_import_impls(TypeChecker *tc, Module *m, ModuleRegistry *reg);

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
  ImplIndex *impls; // the enclosing module's visible set
  InferCtx *infer;  // null during resolve, non-null during check
  // the module whose code is being resolved — the one whose `use` declarations
  // bind the module qualifiers a path may name (`string::len`). Carried here so
  // `resolve_path` reaches it uniformly from resolve, check, and
  // type-annotation contexts, all of which own a TypeResolver.
  Module *module;
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
  Module *m;        // the module being resolved
  ImplIndex *impls; // == &m->visible_impls

  TypeResolver tyres;

  DiagBag *diags;
  Allocator *al;
};

void rctx_init(ResolveCtx *rctx, TypeChecker *tc, Module *m, DiagBag *diags,
               Allocator *al);

static inline Type *rctx_resolve(ResolveCtx *ctx, TypeNode *node) {
  return tyres_resolve(&ctx->tyres, node);
}

// ═══════════════════════════════════════════════════════════════════════════════
// CheckCtx
// ═══════════════════════════════════════════════════════════════════════════════

struct CheckCtx {
  TypeChecker *tc;
  ImplIndex *impls; // the enclosing module's visible set

  // current function
  int loop_depth; // 0 when not in a loop, > 0 inside for/while bodies
  FunDef *fun;
  Type *return_type; // expected return

  // type inference
  InferCtx infer;

  // instantiation records stashed on call nodes for the monomorphiser. Their
  // type arguments are the call's fresh unknowns at the time they are
  // recorded, so they are queued here and rewritten in place by
  // cctx_solve_insts once inference has settled.
  Subst **pending_insts;
  int pending_inst_count, pending_inst_cap;

  // the same, for trait-object coercions: `Expr.coerce_dyn` is a trait
  // reference, and a generic trait's argument can still be an unsolved
  // unknown when the coercion is discovered (`[dyn Into<T>]`).
  Type ***pending_coercions;
  int pending_coercion_count, pending_coercion_cap;

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
  // an item reached through a *type parameter* (`T::from(v)`, `Self::from(v)`
  // in a default body): the trait's signature is all that is known here, and
  // which impl supplies the body waits for monomorphisation — the same answer
  // a method call on a bounded receiver gets.
  PATHRES_TRAIT_ITEM,
  // a method reached through a *fully applied trait reference*
  // (`Into::<Fahrenheit>::into(c)`): the trait names which impl among several
  // for one receiver type, but the self type itself is not written — it comes
  // from the receiver argument. So the path only names (trait ref, method); the
  // impl is selected once the call resolves the receiver. The dual of the
  // milestone-30 overload, which knows the self type and reads the argument.
  PATHRES_TRAIT_QUALIFIED,
} PathResKind;

typedef struct {
  PathResKind kind;
  Type *type;
  union {
    struct {
      FunDef *fun;
      Subst subst; // impl-level substitution (may map to fresh unknowns)
      // non-NULL only for a qualified associated call (`Steps::from(..)`) whose
      // name is declared by more than one impl of a generic trait for this type
      // (`From<Int>` and `From<Char>` for `Steps`). The `fun`/`subst`/`type`
      // above are the first match — enough for a value context — but a call
      // must re-select once its arguments are known. Carries the self type the
      // re-selection needs; NULL means the single-match resolution is final.
      Type *overload_recv;
    } method;
    struct {
      EnumDef *enum_def;
      VariantDef *def;
    } variant;
    struct {
      Type *trait_ref; // TY_TRAIT — the bound the name was found on
      Type *self_type; // the type parameter the path was qualified by
      TraitMethodDef *def;
    } trait_item;
    struct {
      Type
          *trait_ref; // TY_TRAIT — the fully applied trait (`Into<Fahrenheit>`)
      TraitMethodDef *def; // the method the trait declares
    } trait_qualified;
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