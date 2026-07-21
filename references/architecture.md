# ducktape — compiler architecture

Component-by-component internals. Anchors are `file — symbol`; line numbers
shift, symbols don't.

## Scanner (`src/scanner.c`, `include/scanner.h`)

Hand-written; `scanner_tokenise_all` produces the whole token array up front.
Tokens carry a `StringView` lexeme borrowed from the source buffer plus
line/col. String interpolation is lexed with a brace-depth stack
(`interp_braces`): `"a {x} b"` becomes `TOKEN_INTERPOLATION` segments, and a
`}` at the recorded depth resumes string scanning. Logic operators are the
keywords `and`/`or`/`not`; `|` is `TOKEN_PIPE` (closure delimiter only).

## Parser (`src/parser.c`)

Recursive descent with a precedence ladder:
`parse_expr → assign → range → or → and → equality → comparison → addition →
multiply → unary → postfix → primary`. Postfix handles calls, method calls,
field/tuple access, indexing, `as`, `?`.

Notables:
- `parse_block` — statements vs. tail expression. `is_pure_stmt` routes
  `var/return/break/continue/if/for/match/while` through `parse_stmt`, but a
  parsed `if`/`match` statement immediately before `}` is unwrapped into the
  block's tail (that's what makes `{ if c { 1 } else { 2 } }` an `Int` block).
- `parse_closure` — `|params| ( -> type )? ( => expr | { block } )`.
- `parse_type` — `()`, tuples, `fun(..) -> R`, `[T]`, `Self`, named paths,
  and `.Assoc` postfix chains.
- `parse_path(mode)` — expression paths require turbofish (`::<T>`) for type
  args; type paths accept plain `<T>`.
- `p->allow_struct_init` disambiguates `Point { .. }` from a block in `if`/
  `match`/`while` headers.
- Error recovery: `error_at` sets panic mode; `sync_to_stmt`/`sync_to_decl`
  skip to a boundary; unparseable nodes become `*_POISON` kinds.

AST (`include/ast.h`): `Decl`/`Stmt`/`Expr`/`Pattern`/`TypeNode` tagged
unions, plus semantic def tables (`FunDef`, `StructDef`, `EnumDef`,
`TraitDef`, `ImplDef`) that later passes fill in. `Expr.resolved_type` and
`TypeNode.resolved` are stamped by the checker; `FunDef.chunk` and the `slot`
fields are filled by codegen. `EXPR_ASSOCIATED_CALL` is dead — the parser
never produces it.

## Semantic analysis (`src/sema.c`, `include/sema.h`)

Three passes over each module, mirroring compiler phases:

1. **register** — `tc_register_module`: allocate def stubs for
   fun/struct/enum/impl/trait, count-then-populate; `tc_register_builtins`
   adds `print<T>`. `use` is a no-op. (Top-level `var` hits the default
   assert — known gap.)
2. **resolve** — `tc_resolve_module`: resolve signatures and types into
   interned `Type`s; define names in the module's scopes (so declaration
   order matters). Impls resolve self/trait heads, then associated types in a
   pre-pass, then method signatures; `impl_index_add` runs before item
   resolution so `Self.Assoc` is visible to the impl's own methods.
   `resolve_trait_decl` resolves trait item signatures the same way, with
   `Self` bound to the abstract trait type (`ty_trait`): each `TraitMethodDef`
   gets a `method_type` (with `self_index` recording where `self` sits, typed
   as the trait) and a `has_default` flag; a `type Assoc;` becomes an `AssocTypeDef` with a NULL
   type. `Self.Assoc` in a trait signature is left abstract — `TYNODE_ASSOC`
   sees a `TY_TRAIT` base and yields a `ty_assoc` projection instead of going
   through the impl index.
3. **check** — `tc_check_module`: `resolve_expr`/`resolve_stmt` walk bodies.
   `CheckCtx` carries the current function, expected return type, loop depth,
   scopes, and an `InferCtx`. `tc_check_impl` first runs
   `tc_check_impl_conformance` for trait impls: every trait associated type and
   every required method must be present (defaulted methods may be omitted),
   and each method's signature must match. Matching rewrites the trait
   signature into the impl's terms via `trait_project` (abstract `Self` →
   impl self type, `Self.Assoc` → the impl's concrete bound) and compares with
   `types_equal`. `tc_check_trait` then checks each default method *body* —
   once, against the abstract `Self`, not per impl. Extra inherent methods in a
   trait impl are tolerated.

### Types and inference

`Type` (`include/ast.h`) is a tagged union; structural types are interned so
identity is pointer equality (`types_equal`). Singletons: Int, Float, Bool,
String, `()` (unit), `!` (`TY_NEVER` — produced by blocks ending in `return`,
unifies with anything), `Range` (`TY_RANGE`, Int-only), and `TY_POISON`.

Inference is union-find over `TY_UNKNOWN` nodes: `infer_fresh` mints
unknowns, `infer_unify` solves (emitting "type mismatch" diags itself),
`infer_find`/`infer_apply` chase and deep-substitute solutions,
`infer_finalize` reports unsolved unknowns ("cannot infer type"). Generic
instantiation goes through `infer_open_generics`, which builds a `Subst`
(name → type) mapping type params to fresh unknowns or explicit args;
`subst_apply` rewrites types under it.

### Trait bounds

`resolve_generic_param` builds each declaration's type parameter, merging the
bounds written inline (`<T: A + B>`) with any `where T: C` predicate naming it;
`resolve_bound_refs` resolves each trait ref through the type scope (a ref that
isn't a trait is diagnosed) and de-duplicates. The result is a `TY_GENERIC`
carrying `bounds`/`bound_count`.

Enforcement is deferred to the end of inference, because a bound can only be
judged once its parameter is solved. `infer_open_generics` stashes the source
`TY_GENERIC` in each fresh unknown's `.bound`; after `infer_finalize`,
`infer_check_bounds` walks the unknowns, and for every solved one with bounds
asks `impl_index_implements` (does any `impl Trait for T` head match — exact for
a non-generic impl, `impl_type_match` for a generic one) and reports
"type '%s' does not implement trait '%s'" at the unknown's introduction span.
`tc_check_fun`, `tc_check_impl` and `tc_check_trait` all run the finalize +
bound-check pair. An explicitly written type argument skips the fresh unknown
entirely, so `infer_open_generics` records the (param, argument) pair in
`InferCtx.explicit_bounds` and `infer_check_bounds` checks those too.

`ty_generic` interns like every other `ty_*` constructor, canonicalising bound
order first (`T: A + B` and `T: B + A` are one type) — the intern hash is
order-sensitive, so sorting must happen *before* the probe.

### Calls through a trait bound

An *abstract* receiver has no impl to select: a bounded type parameter
(`T: Drawable`), or the `Self` of a trait's own default body. Both dispatch
through trait signatures instead — `resolve_bound_method_call` picks the first
bound declaring the name (`Self` offers its own trait), and
`check_trait_method_call` checks the call against that `TraitMethodDef`. The
signature is written in the trait's terms, so it is rewritten in three steps:
the method's own type params are opened first (`subst_apply` matches generics
by *name*, so doing `Self` first would let a method type param capture the
receiver), then `trait_project` rebases `Self`, then the impl's substitution —
if any — maps the impl's type params to the receiver's type arguments.

The same checker serves an inherited default method: when
`impl_index_method` misses, `impl_index_default_method` looks for an applicable
trait impl whose trait declares the name with a default body, and the call is
checked against the trait signature projected through *that* impl.
`ExprMethodCall.resolved_method` stays NULL in both cases — codegen refuses
them (neither has a chunk until monomorphisation exists).

`impl_applies` is the one place that answers "does this impl apply to this
receiver" (identity for a non-generic impl, `impl_type_match` binding the
params for a generic one); method lookup, default lookup, `impl_index_implements`
and associated-type lookup all go through it.

### Associated-type projections

`T.Assoc` (`TY_ASSOC`, interned like every other structural type) is a
placeholder for whatever an impl binds. `tyres_resolve` accepts it when a bound
on `T` — or the trait itself, for `Self.Assoc` — declares that name.
`subst_apply` substitutes inside the base but cannot collapse the projection
(it has no impl index); `infer_apply` does, reading the binding off the
applicable impl as soon as the base is concrete, and `infer_unify` normalises
`TY_ASSOC` operands up front so `T.Item` and `Int` don't look like different
kinds. While the base is still abstract the projection survives unchanged,
which is exactly what a generic body needs.

**Poison convention:** on error, emit one `diag_error` and return
`t_poison`; poison operands propagate silently so one mistake produces one
diagnostic. Patterns bind their names as poison on bad scrutinees for the
same reason.

### Scopes and name resolution

`ValueScope` (variables/functions; `VarEntry.slot` and `.is_captured` exist
for codegen, capture is flagged when `vscope_lookup` crosses a function
boundary) and `TypeScope` (types; `TypeEntry` carries the def pointer).
Path resolution (`resolve_path` / `cctx_resolve_path`) walks segments through
type scope contexts (struct → associated fn, enum → variant). In
`resolve_callee`, a single-segment path naming a local/value binding is
resolved as a first-class function value; generic functions go through path
resolution so their params are opened into the call's `Subst`.

### Impls and method lookup

`ImplIndex` is a flat list. `impl_index_method` matches a receiver type
against each impl's self type: exact `types_equal` for non-generic impls,
`impl_type_match` (structural, binds the impl's `TY_GENERIC` leaves) for
generic ones. A *bare generic self* — the canonical `Point<T>` that a path
like `Point::new` resolves to, detected by pointer identity with
`def->self_type` — instead selects by method name and opens the impl's params
as unknowns for the call site to solve. That branch is gated on the caller's
`bare_path` flag: since `TY_GENERIC` is interned, a generic impl's `Self` is
pointer-identical to the struct's canonical self, so the type alone can't tell
a bare path from a method receiver. Receivers pass `bare_path=false` — without
it, `self.m()` inside `impl<T> Point<T>` would select any impl defining `m`,
including one for an unrelated concrete instantiation.
`impl_index_assoc_type` does the analogous lookup for `T.Assoc` in type
position.

## Memory (`src/allocator.c`, `src/arena.c`)

Everything flows through the `Allocator` vtable. The compiler owns one arena
(`compiler_init`); tokens, AST, types, scopes, diagnostics, and currently
also bytecode chunks are arena-allocated and freed together in
`compiler_destroy`. That is fine while the VM runs inside the compiler's
lifetime. **Future (GC milestone):** runtime objects (strings, arrays,
structs, closures — and eventually chunks owned by function objects) move to
a mark-sweep GC heap so the frontend arena can be dropped after codegen.

## Diagnostics (`src/diag.c`)

`DiagBag` is a shared side-channel; every pass appends `Diag`s with a `Span`
(line/col range) and the driver prints them with source context after each
phase. Phases report whether errors occurred; `main` exits 0/1 accordingly.
