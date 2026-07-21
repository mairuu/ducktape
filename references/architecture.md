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
- `parse_type` — `()`, tuples, `fun(..) -> R`, `[T]`, `dyn Trait`, `Self`,
  named paths, and `.Assoc` postfix chains.
- `parse_path(mode)` — expression paths require turbofish (`::<T>`) for type
  args; type paths accept plain `<T>`.
- `p->allow_struct_init` disambiguates `Point { .. }` from a block in `if`/
  `match`/`while` headers. It is cleared for the *header only* — inside the
  body the `{` is unambiguous again, so it must be restored before
  `parse_block`.
- Error recovery: `error_at` sets panic mode; `sync_to_stmt`/`sync_to_decl`
  skip to a boundary; unparseable nodes become `*_POISON` kinds.

AST (`include/ast.h`): `Decl`/`Stmt`/`Expr`/`Pattern`/`TypeNode` tagged
unions, plus semantic def tables (`FunDef`, `StructDef`, `EnumDef`,
`TraitDef`, `ImplDef`) that later passes fill in. `Expr.resolved_type` and
`TypeNode.resolved` are stamped by the checker; `FunDef.chunk` is filled by
codegen and the `slot` fields by `exe_link` before it (`runtime.md`).

## Semantic analysis (`src/sema.c`, `include/sema.h`)

Three passes over each module, mirroring compiler phases:

1. **register** — `tc_register_module`: `tc_check_duplicate_decls` first (a
   scope define never detects a collision and a scope lookup returns the
   *first* match, so a redeclaration would silently lose to the original),
   then allocate def stubs for fun/struct/enum/impl/trait,
   count-then-populate; `tc_register_builtins` adds `print<T>`. `use` is a
   no-op here — imports are linked in pass 2, see "Modules". Top-level `var`
   is diagnosed here: every slot space the linker builds is a definition
   table, so a global has nowhere to live.
2. **resolve** — `tc_link_imports` first (see "Modules"), then
   `tc_resolve_module`: resolve signatures and types into
   interned `Type`s; define names in the module's scopes (so declaration
   order matters). Impls resolve self/trait heads, then associated types in a
   pre-pass, then method signatures; `impl_index_add` runs before item
   resolution so `Self.Assoc` is visible to the impl's own methods.
   `resolve_trait_decl` resolves trait item signatures the same way, with
   `Self` bound to the abstract trait type (`ty_trait`): each `TraitMethodDef`
   gets a `method_type` (with `self_index` recording where `self` sits, typed
   as the trait) and a `has_default` flag; a `type Assoc;` becomes an `AssocTypeDef` with a NULL
   type. A method that *has* a default body also gets a `default_impl`: a
   `FunDef` of its own, generic over `Self` (see "Trait default bodies"). `Self.Assoc` in a trait signature is left abstract — `TYNODE_ASSOC`
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
   once, against that method's `default_impl`, not per impl. Extra inherent
   methods in a trait impl are tolerated.

### Modules

**Discovery is fused with parsing** (`compiler_phase_discover`, `src/compiler.c`).
It has to be: a module's dependencies are its `use` declarations, which only
exist once it has an AST. So the phase is a worklist — parse a module, run
`mod_collect_imports` to turn each `use` into a `ModImport` (registering any
file not seen before), and keep going into whatever was just appended. The
loop's bound is `mod_reg.module_count`, re-read each iteration, so those
appends extend it. Two traps worth knowing: `reg->modules` is `al_realloc`'d as
it grows, so the array must never be cached across iterations; and `mod_parse`
calls `diag_clear` on entry, so a module's diagnostics must be reported before
the next one is parsed.

Path handling (`path_dir_of`, `path_normalise`, `mod_file_for_use` in
`src/module.c`) is **lexical, ISO-C only**. The build is `-std=c23`, which
defines `__STRICT_ANSI__` and so hides `realpath`/`getcwd`/`PATH_MAX` behind
glibc's feature macros — and an implicit declaration is a hard error in C23.
Lexical normalisation is sufficient anyway: every module path is a fixed base
dir plus identifier segments, which can't contain `.` or `..`, so two spellings
of one file normalise to identical bytes and `modreg_find` dedups them exactly.
It also works for files that don't exist, which is what lets the missing-module
diagnostic name the path it looked for.

**Cycle detection** (`compiler_phase_dep_graph`) is a tri-colour DFS over the
`ModImport.module_index` edges, appending on post-order so `topo_order` puts
dependencies before dependents. A back edge onto a grey node is a cycle;
`report_cycle` anchors the error at the import that closes it and emits one
`diag_note` per edge rather than building a chain into a fixed buffer — that
accumulation pattern is what produced the `type_sprintf` overflow fixed after
6b. Notes render span-free, so naming modules from several files is safe.

**`tc_link_imports` must run per module in topological order, immediately
before `tc_resolve_module` for that module** — not as a standalone pass, and
not in the register phase where the original stub put it. The derivation:
module-level names are only defined into `m->tscope`/`m->vscope` during
*resolve*, so a dependency has no exports to offer until it has been resolved;
meanwhile the importer's own signatures resolve `Point` through
`tscope_lookup(&m->tscope, ...)`, so its imports must already be in place. The
two constraints only have a solution on a DAG, which is why cycle detection is
a prerequisite rather than a nicety.

Linking finds the item by **scanning the dependency's own top-level decls**
(`mod_find_own_decl`), not by looking in its scopes, then copies the resolved
entry out of the scopes. Scanning the AST is what makes `Decl.is_pub`
readable — visibility is checked here and nowhere else, off the `Decl` rather
than the `*Def` copies — and it is also what stops re-export: a dependency's
scopes also hold *its* imports and the builtins, so a bare `tscope_lookup`
would silently give `use b::X` the meaning of `pub use`. The alternative,
tagging every `VarEntry`/`TypeEntry` with visibility, would have touched 30+
define sites.

`tc_link_imports` also does its **own conflict checking**, because
`vscope_define`/`tscope_define` don't detect duplicates and both lookups return
the first match — an unchecked collision would resolve backwards, letting an
import silently win over the module's own declaration. The `std` case must
short-circuit before that check: `use std::io::print;` names a builtin already
in scope under that exact name, so it is a no-op, and checking it for conflicts
would reject it.

Trait impls are deliberately *not* module-scoped: `ImplIndex` lives on the
`TypeChecker`, shared by every module, so an impl applies program-wide whether
or not its module was imported.

### The embedded standard library

`std/*.dt` are ordinary ducktape sources, mirrored into the binary by
`scripts/embed_std.sh` (one C string literal per line, concatenated by the
compiler) into `build/std_data.h`, which `src/std_src.c` includes as its table.
The `.dt` files stay the source of truth and remain directly runnable; the
generated header lives in `build/`, so `make format` never touches it and
`make clean` removes it.

The design rule is that **a std module is an ordinary `Module`**. `use std::cmp`
resolves to a registry entry exactly like a user import does, and therefore gets
dedup, a dependency-graph edge, cycle detection, topological order, `pub`
checking, linking and codegen for free. `Module.file_path` is used for only
three things — the registry key (`modreg_find`/`modreg_add`), the diagnostic
label (`diag_report`), and `read_file` — and only the third needs a real path.

So the entire difference is **one branch in `mod_parse`**: a `<std>/…` key takes
its source from `std_module_source`, anything else from `read_file`. Pointing
that branch at a directory is all it would take to make std filesystem-backed;
nothing else in the pipeline can tell the difference. `std_mod_key` builds the
key as `<std>/<name>.dt`, and the angle brackets are what guarantee it cannot
collide with a user path: a real one is a base dir plus identifier segments, and
an identifier cannot contain `<`. The key also ignores `base_dir`, so two
modules importing std from different directories dedup to one entry.

`std::io` is the one `std::` path with no module behind it. `print` is
registered into every module's scope by `tc_register_builtins`, so a real
`std::io` exporting it would collide with the builtin already bound under that
name — hence the surviving `is_std` no-op in `mod_collect_imports`,
`compiler.c` and `tc_link_imports`. Every *other* unknown `std::` name is now a
diagnostic listing the embedded modules, where it used to be a silent no-op
that surfaced later as "undefined variable".

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

`Subst` itself lives in `ast.h`, not `sema.h`, because AST nodes store one:
every call node records the substitution that instantiated its target
(`ExprPath.inst`, `ExprMethodCall.inst`) for codegen to monomorphise from.
They are recorded while still holding the call's fresh unknowns and rewritten
in place by `cctx_solve_insts` after `infer_finalize` — see
`runtime.md` "Monomorphisation" for what consumes them.

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

`ExprMethodCall.resolved_method` stays NULL in both cases, so the two are told
apart on the node: a bound call sets `bound_trait` and `bound_self` (the
abstract receiver type), which is what lets codegen redo the impl lookup once
that receiver is concrete; an inherited default sets `resolved_default`, the
`FunDef` of the body it will run. Either way the recorded `inst` binds `Self`
to the receiver's type — see "Trait default bodies".

### Trait default bodies

A default body is checked and compiled *once per receiver type*, like the
generic function it effectively is:

```
trait Show { fun twice(self) -> Int { self.show() + self.show() } }
⇒            fun twice<Self: Show>(self: Self) -> Int { ... }
```

`resolve_trait_default_impl` builds that `FunDef` at resolve time: its type
params are `Self` — `ty_generic("Self", bounds=[the trait])` — followed by the
method's own, and its signature is `method_type` run through `trait_project`
onto that parameter. The trait's `method_type` itself keeps the abstract
`TY_TRAIT` `Self`, since impl conformance and call sites are checked against
it; only the *body* needs a `Self` that a `Subst` — which is keyed by name —
can bind, which is what the milestone turned on.

Everything downstream then falls out of machinery that already existed:
`tc_check_trait` checks the body with `Self` in scope as that type parameter,
so `self.other()` inside it is an ordinary call through a bound, and codegen
instantiates it like any other generic definition.

A type parameter satisfies exactly the bounds it was declared with:
`impl_index_implements` answers for a `TY_GENERIC` from `bounds` rather than
from the index, which is what lets one bounded generic hand its parameter to
another (`fun a<T: Show>(v: T) { b(v) }`). That is sound because the bound is
re-checked against the concrete type wherever the outer function is
instantiated.

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

### Trait objects (`dyn Trait`)

Two type kinds, deliberately not one. `TY_TRAIT` is the *abstract* `Self` of
a trait — bound position, default bodies — and is resolved statically.
`TY_DYN` is a trait object: a real runtime representation, and so `concrete`
as far as codegen is concerned (`type_is_concrete` says yes, and a `dyn Show`
can key an instantiation exactly like a struct can). Sharing one kind would
put two dispatch strategies behind one type.

Resolution is `TYNODE_DYN` → `ty_dyn(trait)`, interned like every other
structural type. **Object safety** is checked there — where `dyn Trait` is
written, not at the trait declaration, because a trait is free to be
static-dispatch-only and only naming it as a type asks for more. A method is
vtable-able only if a receiver and nothing else is enough to call it:
`self` required, no method-level type parameters, and `Self` nowhere but the
receiver (`type_mentions_self` looks past the receiver position).

Dispatch needs nothing new: `resolve_method_call_expr` treats a `TY_DYN`
receiver as offering exactly the one trait it names, and hands it to
`resolve_bound_method_call` like any other abstract receiver. Object safety
is what guarantees the projected signature stays callable once `Self` is
gone. Only *codegen* tells the two apart, by the vtable.

**Coercion** is the one genuinely new concept, and the language's only
subtyping. Identity is pointer equality and `infer_unify` only decomposes
structurally, which is enough everywhere else — but `Sq` and `dyn Shape` are
different runtime representations, and converting between them is a real
operation, not a re-reading of the same bits. So it is modelled as exactly
that: `check_coerce_dyn`, explicit and one-way, attempted only where a value
flows into a *known* `dyn` position (call argument, `return`, a `var` with a
`dyn` annotation, an element of a `[dyn T]` literal) and never as part of
unification. The test is `impl_index_implements` — the same question a bound
asks, since a trait object is a bound whose witness travels with the value.
A `TY_GENERIC` is allowed to coerce, because that function answers a bounded
parameter from its own declared bounds and codegen substitutes the concrete
type before building the vtable.

The result is recorded on the expression (`Expr.coerce_dyn`) rather than
folded into its type, so the node keeps saying what it *is*. That is the same
collect-then-consume shape `inst` uses for monomorphisation, and for the same
reason: at the moment the coercion is discovered the type may still be an
unsolved unknown, and codegen is the consumer either way.

**Poison convention:** on error, emit one `diag_error` and return
`t_poison`; poison operands propagate silently so one mistake produces one
diagnostic. Patterns bind their names as poison on bad scrutinees for the
same reason.

### Scopes and name resolution

`ValueScope` (variables/functions; `VarEntry.slot` and `.is_captured` exist
for codegen, capture is flagged when `vscope_lookup` crosses a function
boundary) and `TypeScope` (types; `TypeEntry` carries the def pointer).
Neither `*_define` detects duplicates and both lookups return the *first*
match, so anything that can collide has to check first — see "Modules" and
`tc_check_duplicate_decls`.
`VarEntry.slot` is meaningless for a module-level binding, imported or not:
the runtime slot lives on the `FunDef` (assigned program-wide by `exe_link`)
and travels with the copied `ve->as`, which is also what codegen reads back
off `ExprPath.resolved_fun` — a name may be an alias for a function in
another module.
Path resolution (`resolve_path` / `cctx_resolve_path`) walks segments through
type scope contexts (struct → associated fn, enum → variant). In
`resolve_callee`, a single-segment path naming a local/value binding is
resolved as a first-class function value; generic functions go through path
resolution so their params are opened into the call's `Subst`.

A bare name that the value scope does not know is looked up in the type scope
before "undefined variable" is emitted: a zero-field struct there means the
name is a unit-struct constructor, and the `EXPR_PATH` node is rewritten into
an empty `EXPR_STRUCT_INIT` and re-resolved — the struct-init path already
handles type arguments and the hint, so a generic `Wrap<Int>` works the same
way. `check_pattern` makes the mirrored rewrite for `PAT_BIND`, so `Marker` in
a pattern is a struct pattern rather than a binding that matches anything.
Both consult the value scope first, which keeps an ordinary variable winning —
though the pattern rewrite means such a variable can no longer be declared.

### Match exhaustiveness

`check_match_exhaustive` (`src/sema.c`) runs at the end of
`resolve_match_expr`, once every pattern has been checked — it reads the
`resolved_type` and the resolved struct/variant defs the pattern check stamped
onto the patterns.

It is Maranget's usefulness algorithm narrowed to the one question a match
asks: is the all-wildcard row still useful against the arms? The matrix holds
one row per arm and one column per value left to discriminate; a NULL column
is a wildcard — a `_`, a binding, or a field the pattern simply omitted. For
column 0 the checker takes the type from the first row that constrains it and
asks for that type's *signature*: both `Bool` literals, every variant of an
enum, the sole constructor of a struct or tuple. If the arms mention every
constructor in the signature, it specialises (`S`) by each in turn and
recurses; otherwise it drops to the default matrix (`D`) and recurses on the
remaining columns, which is exhaustive only if some row was a wildcard there.

Two decisions are load-bearing:

- **The column type comes off the pattern, not the field declaration.** A
  field declared `T` is concrete at the match site — `Opt<Bool>`'s payload has
  two values, not an unenumerable domain. Reading `FieldDef.type` would report
  a correct `Opt::Some(true) | Opt::Some(false) | Opt::None` as a gap.
- **The answer is tri-state** (`EXH_YES` / `EXH_NO` / `EXH_UNKNOWN`). A column
  whose type inference has not pinned down (an unsolved unknown, a generic, a
  projection) has an unknowable domain, so nothing is reported: a false "not
  exhaustive" rejects a correct program, which is worse than the
  `OP_MATCH_FAIL` backstop the VM already has.

A guarded arm contributes no row at all — whether it matches is a runtime
question, so it can never be what makes a match exhaustive. That is also why
`OP_MATCH_FAIL` stays reachable (`runtime.md`).

### Binding patterns (`var`)

A `var` binding is the same `Pattern` a match arm carries — there is no
separate binding grammar. `check_binding_pattern` runs `check_pattern` against
the initializer's type (which defines every name the pattern binds into the
enclosing scope, exactly as a match arm does) and then asks the *same*
exhaustiveness question over a one-row matrix: a binding is a match with one
arm and no guard, so irrefutable is precisely exhaustive. `EXH_NO` is the
"refutable pattern in a 'var' binding" diagnostic; `EXH_UNKNOWN` reports
nothing, for the reason above.

When the pattern cannot be checked — a poisoned initializer, or a mismatch
that stopped the walk partway — `bind_pattern_poison` defines the remaining
names as poison. `vscope_lookup` returns the *first* entry it finds, so the
correctly-typed names an aborted `check_pattern` already defined still win,
and only the ones it never reached come back poisoned. Without it one bad
initializer becomes an "undefined variable" for every name it was meant to
bind.

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
