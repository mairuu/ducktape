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
3. **check** — `tc_check_module`: `resolve_expr`/`resolve_stmt` walk bodies.
   `CheckCtx` carries the current function, expected return type, loop depth,
   scopes, and an `InferCtx`.

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
as unknowns for the call site to solve. `impl_index_assoc_type` does the
analogous lookup for `T.Assoc` in type position.

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
