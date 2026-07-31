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

A character literal is `TOKEN_CHAR`, and it is the one token whose *content*
the scanner validates rather than merely delimits. Finding the end needs escape
awareness anyway (`'\''` holds a quote), and once the escapes are being read
there is no reason for a second reader: `char_literal_value` (`scanner.h`)
decodes a lexeme into a scalar value and returns the message describing what is
wrong with it, so the scanner reports and the parser re-reads a lexeme it knows
is good. The alternative — a payload field on `Token` for one token kind —
would widen every token in the array to carry a value only this one has.

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
- `parse_closure` — `|params| ( -> type )? ( { block } | expr )`. The body has
  no introducer token: a `{` is read as the block form and anything else starts
  an expression, so the two are told apart by lookahead rather than by a `=>`.
- `parse_type` — `()`, tuples, `fun(..) -> R`, `[T]`, `dyn Trait`, `Self`,
  named paths, and `.Assoc` postfix chains.
- `parse_path(mode)` — expression paths require turbofish (`::<T>`) for type
  args; type paths accept plain `<T>`. A method call's type arguments
  (`it.fold::<Int>(0, f)`) take the turbofish for the same reason and are read
  by the same `parse_type_args`, in `parse_postfix` rather than in the path.
  That is what keeps a `<` after a field or method access unambiguous: the
  `::` decides between a type-argument list and the less-than operator before
  either side is parsed, so no lookahead is needed and none is done.
- `p->allow_struct_init` disambiguates `Point { .. }` from a block in `if`/
  `match`/`while` headers. It is cleared for the *header only* — inside the
  body the `{` is unambiguous again, so it must be restored before
  `parse_block`.
- Error recovery: `error_at` sets panic mode; `sync_to_stmt`/`sync_to_decl`
  skip to a boundary; unparseable nodes become `*_POISON` kinds.
- **Every list buffer grows; none of them caps** (milestone 73). One facility,
  `PLIST`/`PLIST_GROW`/`PLIST_PUSH`/`PLIST_TAKE` at the top of `parser.c`,
  backs all of them: arguments, match arms, struct fields, enum variants,
  tuple elements, bounds, path segments, trait items, interpolation segments,
  a block's statements, an impl's items, a module's declarations. A list
  accumulates into a zeroed stack buffer and *spills* to the arena when that
  fills; `PLIST_TAKE` right-sizes it into the node.

  The stack buffer is a small-size optimisation, not a ceiling, and it stays
  for a reason particular to this allocator: `arena_realloc_fn` bump-allocates
  a fresh block and abandons the old, so an always-arena list would cost ~2n
  doubling plus n for the right-sized copy where the stack version costs n.

  Before this, twenty-odd sites each carried their own cap — 16 for most, 8
  for bounds and path segments, 64 for a trait's items — and each reported
  "too many ...", i.e. refused a program the grammar accepts. Two were worse
  than a refusal: a module's declarations were a 1024-entry array guarded by
  an `assert`, which the release build's `NDEBUG` deleted (milestone 72, fixed
  there), and **a method call's arguments were a `tmp[64]` with no check at
  all** — `obj.m(a1, ..., a65)` wrote past it and smashed the stack, caught
  here only by the toolchain's default stack protector (milestone 73).

  `MAX_ASSOC_BINDINGS` survives as an inline size only. As a *limit* it was
  answering a question the parser cannot answer: whether a `dyn Trait<..>`
  binding list is total is decided against the trait's own associated types,
  which `resolve_dyn_type` does by name.

AST (`include/ast.h`): `Decl`/`Stmt`/`Expr`/`Pattern`/`TypeNode` tagged
unions, plus semantic def tables (`FunDef`, `StructDef`, `EnumDef`,
`TraitDef`, `ImplDef`) that later passes fill in. `Expr.resolved_type` and
`TypeNode.resolved` are stamped by the checker; `FunDef.chunk` is filled by
codegen, which also hands out the `slot` fields as it reaches definitions
(`runtime.md`).

## Semantic analysis (`src/sema.c`, `include/sema.h`)

Three passes over each module, mirroring compiler phases:

1. **register** — `tc_register_module`: `tc_check_duplicate_decls` first (a
   scope define never detects a collision and a scope lookup returns the
   *first* match, so a redeclaration would silently lose to the original),
   then allocate def stubs for fun/struct/enum/impl/trait,
   count-then-populate. A top-level `fun` carrying `@native`/`@intrinsic` is
   bound to C's registry here by `tc_bind_native` — the attribute still has a
   span, so an unknown name reports against it (`runtime.md` "Native
   functions"); an impl method's attribute is bound by the same call in the
   *resolve* pass, where the method's `FunDef` is built (milestone 39). There
   are no builtins: `print` is an ordinary import of `std::io`. `use` is a
   no-op here — imports are linked in pass 2, see "Modules". Top-level `var`
   is diagnosed here: every slot space the linker builds is a definition
   table, so a global has nowhere to live.
2. **resolve** — `tc_link_imports` first (see "Modules"), then
   `tc_resolve_module`: resolve signatures and types into interned `Type`s.
   This is itself **three sub-passes** so declaration order does *not* matter: a
   **declare** pass (`declare_struct_decl`/`declare_enum_decl`/
   `declare_trait_decl`) binds every top-level type *name* to a head type (and,
   for a trait, records its associated-type *names* — a straight copy off the
   AST); a **supertrait** pass (`resolve_trait_supers`, then
   `trait_build_closure`) resolves each trait's `: A + B` list and flattens it;
   then a **body** pass fills fields, variants, method signatures, impls and
   functions — each free to name a type declared later, including a mutual
   reference (a trait method returning an adapter struct whose own bound is
   that trait). The supertrait pass sits between the other two because a
   signature may project through a super (`Self.Item` where `Item` is the
   super's), so every trait's supers have to be known before any signature is
   resolved — see "Supertraits". A definition's type parameters are resolved once, in the
   declare pass; the body pass re-enters them by name (`redeclare_type_params`)
   rather than re-resolving their bounds, so a bad bound is diagnosed once.
   The one thing still order-sensitive is a type-parameter *bound* itself,
   since it is resolved in the declare pass (a `struct Map<I: Trait>` needs
   `Trait` declared earlier). Impls resolve self/trait heads, then associated
   types in a pre-pass, then method signatures; `impl_index_add` runs before
   item resolution so `Self.Assoc` is visible to the impl's own methods.
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
readable — *item* visibility is checked here, off the `Decl` rather
than the `*Def` copies — and it is also what keeps re-export *deliberate*: a
dependency's scopes also hold *its* imports, so a bare `tscope_lookup` would
give every `use b::X` the meaning of `pub use`. The alternative, tagging every
`VarEntry`/`TypeEntry` with visibility, would have touched 30+ define sites.

**`pub use` is then one extra lookup on the same footing.** When
`mod_find_own_decl` misses, `mod_find_use_alias` scans the dependency's
`DECL_USE` nodes for one binding that alias; `Decl.is_pub` on the *use* decl is
what distinguishes a re-export from a private import, so the same visibility
rule applies unchanged and the two failures get different wordings ("is private
in module" vs "is imported by module ... but not re-exported"). Nothing else
moves: the entry being copied is already in the dependency's scopes, put there
when *it* was linked, which topological order guarantees has happened.

**Field visibility is the one check that is *not* at the import boundary.**
A struct field's `pub` (`FieldDef.is_pub`) gates access from another module, but
a field is never imported by name — it is reached through a value — so the check
has to live at each *use*, not at the `use`. All three uses funnel through
`find_struct_field`, so a single guard, `check_field_visible`, sits at each of
its callers: construction (`EXPR_STRUCT_INIT`), `.field` access-or-assignment
(`EXPR_FIELD`, which a write reaches through its assignment target), and a struct
pattern. The rule is `field->is_pub || accessing_module == def->module`, the
accessing module read off `CheckCtx.tyres.module`; a same-module access ignores
`pub` entirely. Codegen and the bytecode image are untouched — field *indices*
do not change — so this is a pure front-end check.

`tc_link_imports` also does its **own conflict checking**, because
`vscope_define`/`tscope_define` don't detect duplicates and both lookups return
the first match — an unchecked collision would resolve backwards, letting an
import silently win over the module's own declaration. There are two import
kinds now: an item import copies entries into the scopes (above), and a *module
import* binds a qualifier (below).

**The prelude is discovery, not a new phase** (`mod_inject_prelude`,
`src/module.c`; milestone 45). ducktape has no auto-import, so a construct whose
meaning is a lang item used to need an import the user never wrote — `"{v}"` on
a struct needed `std::fmt`, `a < b` on a struct needed `std::cmp`. The prelude
removes that class of friction by making every *non-std* module implicitly
depend on a fixed set: `std::option` (`Option`), `std::result` (`Result`),
`std::cmp` (`Ord`), `std::fmt` (`Display`), and `std::string` (capture-only, for
the `pad_*` a width spec needs). The mechanism reuses everything above: after
`mod_collect_imports`, the discovery loop appends **synthesised `ModImport`s** —
ordinary `DECL_USE` nodes with `from_prelude` set — to `m->imports`, registering
any prelude module not yet seen. The dep graph, register, resolve, and link
phases are untouched; they already iterate `imports[]`. Three properties make it
safe:

- **std modules are exempt** (`is_std_key`): a prelude module cannot import
  itself, which is the only cycle the exemption avoids.
- **The prelude is lowest priority.** Its entries are appended *after* a
  module's real imports, so those link first; and on a name already bound — an
  own decl or an explicit import — `link_import_item` sees `from_prelude` and
  **yields silently** (`link_name_bound`, no diagnostic) where an explicit
  import would report the clash. This is what keeps a hand-written `enum Result`
  (or `Option`) working: the prelude's just steps aside. It is Rust's
  "a local item shadows the prelude" rule.
- **Lang-item capture falls out for free.** `display_trait`/`ord_trait`/
  `fmt_pad_*`/`fmt_float` are captured when their module *registers*, and the
  prelude guarantees it always does — so those fields (see `sema.h`) went from
  "NULL unless the user imported it" to always-populated. The "you forgot to
  import `std::fmt`" diagnostics are now unreachable; their NULL guards remain
  only as defence against a std module failing to load. `print` is deliberately
  *not* preluded — it is an ordinary function tied to no syntax, so it stays an
  explicit `use std::io::print`.

### Module-qualified paths

`use a::b;` naming a module binds `b` as a **qualifier**, so `b::thing`
resolves against `b`'s exports. The observation is that an item import and a
qualifier are the same reachability with two spellings: `use` already loads the
file, adds the dependency edge, and imports the impls — a qualifier only adds a
*name for the module*, so nothing about linking, the graph, or `impl` visibility
changes. It splits across the same three seams every module feature does:

- **Discovery decides the shape** (`mod_collect_imports`). A braced `use` is
  always an item import. A bare one is ambiguous — `use a::b;` is either item
  `b` of module `a` or module `a::b` — and only file existence disambiguates, so
  the parser keeps the whole path (`DeclUse.bare`) and collection resolves it:
  if the full path names a module file it is a module import
  (`is_module_import`), otherwise the trailing segment is an item of the prefix.
  A std module is exactly `std::<leaf>`, so its module/item split is by segment
  count rather than a filesystem probe.

- **Linking binds the name** (`link_bind_module`). A module is not a value or a
  type, so it lives in neither scope: `Module.qual_modules` is its own small
  table of `{name, target Module*, span}`, filled beside the item imports and in
  the same topological order. The collision check is the reused `link_name_taken`
  plus a scan of the qualifiers already bound — a qualifier shares the item
  namespaces' names precisely so `string` cannot mean two things.

- **Resolution adds one state** (`resolve_path`). The first segment, when it is
  neither a type nor a builtin, is looked up in the importer's `qual_modules`
  (reached through `TypeResolver.module`, which every resolve/check/annotation
  context now carries). A hit enters `PATHRES_CTX_MODULE`, whose next segment is
  found by `module_export_lookup` — the same `mod_find_own_decl` /
  `mod_find_use_alias` + `is_pub` gate `link_import_item` uses, so a qualified
  path and an item import see exactly the same set — and then handed to
  `pathres_step_entry`, the factored-out body of the scope state's type-entry
  switch. So `a::Point::new` reuses the struct→associated-fn machinery, and a
  qualified function resolves to the same `PATHRES_METHOD` a bare call does:
  codegen reads `ExprPath.resolved_fun` and never learns a module was involved.

### Where an `impl` applies

An impl has no name, so it cannot travel through `use` one item at a time.
**Reachability is the handle instead**: `Module.visible_impls` holds every impl
the module may select from — its own, plus those of every module reachable
through `use`, transitively.

It is a **union, not a filter**. `tc_import_impls` copies each dependency's
*visible* set into the importer (deduped by `ImplDef` pointer, so a diamond
import adds nothing twice), running alongside `tc_link_imports` in topological
order and for the same reason: a dependency's own impls are only registered as
*it* is resolved. Building a set per module rather than filtering one global
list is what lets every `impl_index_*` lookup keep the signature it had — only
the argument changed, from `&tc->impl_index` to a `ResolveCtx`/`CheckCtx` field.
The set is transitive because `pub use` can re-export a type whose impls live
one module further away.

**Coherence replaces first-registration-wins.** `impl_defs_conflict` asks
whether two impls head the same trait *and* either one's `impl_applies` accepts
the other's self type — selection's own question, asked from both sides, so
`Ord for Option<T>` conflicts with `Ord for Option<Int>`. A conflict is
reported twice over, at the two places a pair can first meet: in
`resolve_impl_decl` when a module's own impl meets an imported one, and in
`tc_import_impls` when one `use` makes two dependencies' impls visible at once.
Both spans lie in the module being compiled, which is required — diagnostics
are reported against that module's source.

**Inherent impls get the same treatment at the granularity of a name.**
Splitting a type's methods across several blocks is ordinary, so the impl head
is the wrong unit; `impl_defs_share_method` asks the overlap question with
`trait_ref` NULL (the receiver alone, both directions, bounds ignored) and then
looks for a method name both sides declare *inherently*. `method_is_inherent`
answers that: a name the impl's trait declares — `trait_flat_method_index` ≥ 0,
so a supertrait's methods count — is reached through a bound or a
trait-qualified path and may repeat, while everything else, including the extra
methods a trait impl carries beyond its trait, is reached by the receiver alone
and may not.

Two placement consequences. The check runs at the *end* of `resolve_impl_decl`
rather than beside the trait one at the top: `tc_register_impl` allocates
`ImplDef.methods` zeroed and the names are filled while the items resolve, so a
name-granular question has nothing to ask until then. And an impl cannot
conflict with itself, so two methods of one name in one block are a separate
pairwise loop over `impl_def->methods` with its own diagnostic.

`TypeChecker.all_impls` is the one program-wide list that survives, and it is
**never selected from**. It exists so a failure can explain itself:
`find_unimported_impl` looks for an impl that would have applied but is not in
the visible set, and three diagnostics (missing method, unsatisfied bound,
non-`Display` interpolation) append "an applicable impl exists in module 'x',
but this module does not import it". Without it, making impls module-granular
would have turned a working program into "no method named 'show'".

### An impl's own bounds, at selection

`impl_applies` answers two questions, not one. The first is structural — does
the impl's self type match the receiver, binding the impl's type params
(`impl_type_match`). The second, added once std shipped a container `Display`,
is whether those bindings satisfy the bounds the impl *declared on them*:
`impl<T: Display> Display for [T]` applies to `[Int]` and not to `[Widget]`.

Asking at selection is the whole point. Left unasked, the impl is selected
regardless and the bound is felt only inside its body — where the diagnostic
names the *impl's* source rather than the code that reached for it. For a std
impl that means pointing into `<std>/fmt.dt`, at codegen, about a method call
the user never wrote.

The check is opt-in per call site, through `impl_applies`'s `bounds_idx`
parameter (NULL = structural only):

- the three **selection** paths pass the visible set — `impl_index_method`,
  `impl_index_default_method`, `impl_index_implements`;
- **coherence** (`impl_defs_conflict`) passes NULL, deliberately. Two impls for
  one type overlap whether or not their bounds are disjoint, and the
  conservative answer is the one coherence wants. It also runs during resolve,
  while the index is still filling;
- **associated-type lookup** (`impl_index_assoc_type`) passes NULL for the
  second of those reasons alone.

Answering **recurses**: `[[Int]]` asks whether `[Int]` is `Display`, which
selects the same impl one level down. The type shrinks each step, so the only
non-terminating shape is a self-referential blanket impl
(`impl<T: Foo> Foo for T`) — caught by `IMPL_BOUND_MAX_DEPTH`, past which an
impl simply does not apply.

`note_blocking_bound` is the counterpart to `find_unimported_impl`, and exists
for the same reason: an impl that matched structurally but was rejected for a
bound would otherwise report only that `[Widget]` is not `Display` — true, and
useless, since the impl exists and the fix is one level down. The note names
it (`an impl exists, but it requires 'Widget: Display'`) and is appended to the
same three diagnostics.

A bound is read **in the terms the head just pinned**. `impl<T, U: From<T>>`
declares a bound mentioning another of the impl's own parameters, so asking
whether `Meters` implements a literal `From<T>` answers no however many impls
exist; `impl_bounds_satisfied` substitutes the head's bindings into the bound
first (partially — a bound may also mention the *enclosing* definition's
parameters, which this substitution has nothing to say about). That is the
same move `infer_open_generics` makes for a function's bounds.

### An impl parameter the head does not pin

`impl_applies` requires every impl parameter to be pinned by the self type or
the trait reference. A conversion is the one shape where that is impossible by
construction: `impl<T, U: From<T>> Into<U> for T` is selected by neither its
receiver nor a written reference, because *what a value converts into is not a
fact about the value*. So `impl_index_method` splits the question in two —
`impl_match_head` binds what the head mentions, and a parameter still unbound
is pinned by matching the candidate method's return type against the call's
expected type (`ret_hint`), after which `impl_head_complete` checks totality
and the impl's own bounds.

Three properties keep that from becoming a selection rule of its own:

- it is consulted **only to fill a hole**. With the head already total the hint
  stays the tie-break milestone 28 added, so a wrong hint still reports the
  ordinary argument mismatch against the one impl that exists rather than
  "no method";
- the method must have **no type parameters of its own**, since its return
  type would then mention generics this substitution does not bind;
- what the hole is filled with is **verified, not trusted** — the impl's
  `U: From<T>` bound is asked afterwards, against the type the hint supplied.
  That is what makes `26.into()` and `'a'.into()` reach different `From` impls
  for one `Steps`.

The method is therefore looked up *before* the impl is judged applicable,
which is the one structural change: a candidate's signature is now part of
deciding whether its impl applies at all.

### Selection by argument type

The tie-break above pins an open parameter by the type a call is *expected to
produce*. Its mirror is the type a call *consumes*, and the two together are
what `into` and `from` need respectively. `26.into()` is pinned by its receiver
before the impl's bound is asked; `Steps::from(26)` has no receiver, and the
qualified path names the produced type (`Steps`) but not the source — so when a
generic trait is implemented for one type more than once (`From<Int>` and
`From<Char>` for `Steps`) neither the self type nor a written reference can
choose between the impls. The argument is the only thing that can, and until
milestone 30 selection never read one: the first registered impl won and every
other argument was a type error against it.

The obstacle is ordering. `resolve_callee` resolves the path — and thus selects
the impl — *before* the arguments are checked, because normally the parameter
types are the hint the arguments are resolved against. Reading the arguments to
choose the impl inverts that, so the ambiguous case is split out rather than
folded in:

- **`cctx_resolve_path` only flags the ambiguity.** In `PATHRES_CTX_TYPE` it
  still returns the first match — all a value context can use — but
  `assoc_candidate_count` counts the impls declaring the name for the self type,
  and more than one sets `PathRes.overload_recv`. The bare generic self of
  `Point::new` is excluded, since it selects by method name, not by a trait
  argument.
- **`resolve_callee` declines to commit** when it sees the flag: it hands the
  self type and name back through an `AssocOverload` out-param and returns
  without building a callee type.
- **`resolve_assoc_call` finishes the job**, and it is the one place arguments
  precede selection. The arguments are resolved *hint-free* — the argument is
  precisely what is meant to decide the impl, so a value that would need the
  parameter as a hint cannot disambiguate anyway — and `impl_index_assoc_select`
  picks the one impl whose method accepts them. Resolving the arguments here,
  once, is also what keeps them from being resolved twice: the same types feed
  both selection and the after-the-fact argument check.

`impl_index_assoc_select` reuses the head machinery unchanged. Each candidate
method's parameter is a pattern in the impl's terms, and `impl_type_match` — the
same non-committing matcher the head uses — tests it against the resolved
argument, binding any impl parameters the arguments mention; a hole the
arguments leave is then filled by `ret_hint` exactly as the return-type
tie-break does, and `impl_head_complete` checks totality and bounds. A unique
accepting impl wins; none is a failure that names the argument types and lists
what each candidate takes, and more than one is the ambiguity coherence makes
practically unreachable but the selector still refuses. As with the return-type
case, only a method with no type parameters of its own takes part, for the same
reason: its signature would otherwise mention generics this match cannot bind.
The resolved node is then indistinguishable from an ordinary associated call —
a concrete `resolved_fun` and the impl's substitution as its instance — so
codegen needs no new case.

#### The receiver spelling (milestone 75)

The same selector now also runs for `v.scale(10)`, and it had to: `a * b`
desugars to `a.mul(b)`, so a heterogeneous operator is a *receiver* call, and
the receiver spelling still had the pre-milestone-30 behaviour — the first
registered impl won and every other argument was an "expected 'Int' but got
'V2'" against it.

`resolve_method_call_typed` has no ordering obstacle to work around, because it
already holds the receiver's type: it pre-resolves the arguments hint-free into
`selected_args` (the receiver first, since a method's signature carries `self`
as an ordinary parameter), calls `impl_index_assoc_select`, and then hands the
*same* types to the argument-checking loop below rather than resolving them a
second time — which would re-report a bad argument and re-queue every
instantiation inside it.

The interesting part is the **guard**, and it is a different question from "is
there more than one candidate". `assoc_candidates_differ_in_args` asks whether
the candidates disagree about the arguments they *take*, comparing parameter
lists only:

- `Into<Fahrenheit>` and `Into<String>` for `Celsius` both take `(Celsius)` and
  differ in their return type. The return-type tie-break is what settles them,
  and running argument selection would report an ambiguity instead.
- A supertrait-derived impl (milestone 74) shares the written impl's `MethodDef`
  outright, so its signature is the same pointer.

Comparing the *return* type as well would put both of those back — the first
directly, and it is why `sigs_take_same_args` stops at the parameters. So the
rule is: arguments decide only where arguments can decide, and every older
tie-break keeps the cases it already had.

### Trait-qualified calls

`Steps::from(v)` above reads the *argument* to pick among several impls, because
the self type is known and the trait reference is not. `Into::<Fahrenheit>::into(c)`
is the exact dual: the trait reference is *written* (`Into<Fahrenheit>`) and the
self type is not — it is the receiver argument. A bare `c.into()` cannot say
which impl when `Celsius` goes `Into` two ways; naming the trait can. And
`(self type, trait reference)` is exactly the pair `impl_index_method` has keyed
on since generic traits (milestone 28), so the whole feature is teaching the
path grammar to reach that lookup — the parser already produces the two segments
(`[Into<Fahrenheit>, into]`), turbofish and all.

The ordering is the milestone-30 constraint one field over — the callee cannot
be selected before its receiver is resolved — so the ambiguous case is again
split out rather than folded in:

- **`resolve_path` only names the trait and method.** A `TY_TRAIT` segment that
  is *not* last stops being "cannot access member of a trait" and instead
  transitions into `PATHRES_CTX_TRAIT` carrying the resolved trait reference;
  the final segment is looked up among the trait's methods and returned as
  `PATHRES_TRAIT_QUALIFIED` (trait reference + `TraitMethodDef`). No impl is
  chosen: the self type is not written here.
- **`resolve_callee` declines to commit**, handing the trait reference and
  method back through the `CalleeDefer` out-param — the same struct the
  argument-selection case fills, one field over.
- **`resolve_trait_qualified_call` is the one place the receiver precedes
  selection.** It resolves the argument at the method's `self_index` hint-free
  (a receiver whose type is still unknown cannot select, the dual of "the
  argument must type on its own"), then `impl_index_method(self, trait_ref, name)`
  picks the concrete impl and the arguments are checked against its signature —
  the receiver among them, positionally, since a trait-qualified path is the
  spelling where a method's `self` is an ordinary first parameter.

No hint is passed to the selector: the reference already names every trait
argument, so there is no hole for a return-type tie-break to fill. The resolved
node is a concrete `resolved_fun` with the impl's substitution as its instance
— identical to what `Steps::from` produces — so codegen is again untouched, and
a generic impl (the `std::convert` blanket) monomorphises exactly as `x.into()`
does. A method that carries type parameters of its *own* (`fun wrap<U>(..)`) is
no obstacle: the impl was chosen by `(receiver, trait reference)`, so those
parameters play no part in selection and are simply opened as fresh unknowns —
turbofish on the final segment supplying them if written — the same handling a
receiver method call gives them.

Three things are deliberately refused, each with a diagnostic that names the
alternative rather than the internals:

- an **associated function** (`from`, no `self`) has no receiver to select by —
  the qualified `Type::from(..)` spelling is where a source type is written down;
- a method the impl **inherits from a default body** rather than defining needs
  the bound-dispatch machinery a receiver call has and this concrete path does
  not, so the diagnostic points at `x.method(..)`;
- an **abstract receiver** (a bounded type parameter) has no concrete impl to
  resolve to; `s.into()` already dispatches through its bound unambiguously, so
  the trait-qualified spelling adds nothing there.

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

Every `std::` name resolves to a module or is a diagnostic listing the embedded
ones. `std::io` used to be the exception — a no-op namespace over the builtin
`print`, carried by an `is_std` flag through `mod_collect_imports`,
`compiler.c` and `tc_link_imports` — and it is now a real module like the rest,
so the flag and all three of its readers are gone.

### Interpolation and `Display`

`"{v}"` has to turn `v` into a String, and the question that shapes the whole
design is *who decides how*. Two answers, and both are used:

- **A primitive renders itself.** Int, Float, Bool and String are handled by
  the VM's `stringify` (`src/vm.c`), which has known how since the feature
  existed — Char joined them in milestone 26, rendered by `utf8_encode`.
  `check_interpol_seg` lets those five through untouched — no call is emitted
  at all — and that is what lets a program interpolate a number without
  importing anything.
- **Anything else asks the type**, through `std::fmt`'s `Display`.

Once the trait is what decides, the segment *is* the call `v.to_string()`. So
that is what `check_interpol_seg` rewrites it into: it builds an
`EXPR_METHOD_CALL` node around the segment expression and resolves it. The
consequence is the whole point — dispatch through a trait bound, through a
`dyn`, an inherited default body, and monomorphising the instance all arrive
already working, and **codegen, `OP_INTERP`, the VM and the image format need
no change at all**, because the segment now simply evaluates to a String, which
`stringify` passes straight through.

Two details make the rewrite safe:

- The receiver has to be resolved *first*, to know whether it is a primitive at
  all — so the rewrite cannot go through `resolve_method_call_expr`, which
  starts by resolving the receiver. `resolve_method_call_typed` is that
  function minus its first three lines, and re-resolving instead would
  re-report the receiver's diagnostics and re-queue its instantiations.
- The `Display` check runs *before* the rewrite rather than letting method
  resolution fail. That is what keeps the diagnostic about interpolation
  ("cannot interpolate a value of type 'Point': it does not implement
  'Display'", or the bound to add for a type parameter) instead of about a
  missing method, and it is also what stops an unrelated inherent `to_string`
  from silently qualifying.

**`Display` is a lang item the compiler tracks** — `TypeChecker.display_trait`,
captured in `tc_register_trait`. Since the prelude imports `std::fmt` into every
program (see "Modules"), it is always populated; the NULL guard survives only as
defence against a std module failing to load. A user trait called `Display` is
an ordinary trait — the capture is gated to the standard library, not keyed on
the spelling (see "Lang items"), so interpolation never routes through a user's.

`display_satisfied` asks `impl_index_implements`, the same question a trait
bound and a `dyn` coercion ask — so a bounded type parameter answers from its
declared bounds and is re-checked where it is instantiated. The two abstract
kinds the impl index has no entry for, `TY_DYN` and the `TY_TRAIT` of a default
body, answer from the trait they name.

**A format spec desugars in the same place.** A `{v:>8}` / `{f:.3}` / `{f:>8.3}`
spec (milestone 35) is parsed into a `FormatSpec` on the `InterpolSeg` — one
alignment (`<` `>` `^`) with a width, an optional fill char, and an optional
`.N` precision, or a `.N` alone — and `check_interpol_seg` rewrites the segment
into ordinary calls: the value is rendered to a `String` (through
`std::fmt::float` for a precision, through a nested `{v}` interpolation
otherwise, so a primitive and a `Display` type render the same way they always
do), and if a width was given the render is wrapped in the matching
`std::string::pad_start`/`pad_end`/`pad_center`. Nothing downstream of the
checker sees a `FormatSpec`: the segment simply evaluates to a `String`, so
codegen, `OP_INTERP`, the VM and the image format are untouched — the same shape
the `to_string` rewrite has.

The desugaring's callees are captured as lang items exactly like `Display`, and
for the same reason: the user never writes `pad_start` or `float`, so leaving
those names to whatever is imported would make the meaning of `{v:>8}` depend on
imports. `tc_register_fun` captures `TypeChecker.fmt_pad_start`/`fmt_pad_end`/
`fmt_pad_center` (in `<std>/string.dt`) and `fmt_float` (in `<std>/fmt.dt`) from
their `@lang` markers (see "Lang items"); the prelude imports both modules, so
all four are always present. This is the crack in "the compiler knows nothing
about std" that `Display` opened widened to five names, and the honest framing is
that formatting as a whole — deciding how a value renders *and* how it is laid
out — is the corner of the standard library the language is entangled with. The
calls themselves are
built by `mk_fun_call`, which sets `resolved_fun` directly on a synthetic
two-segment path (so codegen skips the local-name lookup) rather than routing
through `resolve_callee`, which resolves by name and would fail.

The `pad_*` are non-generic ducktape functions and `float` a non-generic native,
so the calls need no inference — the render is a `String`, the width and
precision `Int` literals, the fill a `Char` literal, all correct by
construction, which is why the rewrite validates only the value's own type (a
precision requires a `Float`).

### Ordering operators and `Ord`

`<`/`<=`/`>`/`>=` are the same move as interpolation, one operator over
(milestone 38). The `EXPR_BINARY` case in `resolve_expr` keeps the built-in path
for two numeric operands — `type_is_numeric(lhs) && type_is_numeric(rhs)` emits
`OP_LT`/etc. with no import and no frame, exactly as a primitive interpolation
segment keeps the VM's own rendering. A non-numeric operand routes through
`rewrite_ord_comparison`, which reshapes `a OP b` into `a.cmp(b) OP 0` in place:
it builds an `EXPR_METHOD_CALL` for `a.cmp(b)`, resolves it with
`resolve_method_call_typed` (the receiver type is already known, so it must not
re-resolve), then sets `binary->left` to that call and `binary->right` to a `0`
literal, leaving the operator untouched. The outer node is now an `Int OP Int`
comparison — so codegen, `OP_LT` and the VM need no change, the mirror of the
`to_string` rewrite evaluating to a `String`.

Two things carry over from the `Display` design intact:

- **Only `cmp` is named.** Rewriting to `cmp` compared to `0` — rather than to
  `Ord`'s `lt`/`le`/`gt`/`ge` defaults — keeps the lang-item surface a single
  method, the way interpolation names exactly `to_string`. The four default
  methods stay ordinary trait items, callable directly (`a.lt(b)`), just not
  what the operator reaches for.
- **`ord_satisfied` gates the rewrite**, for the same two reasons
  `display_satisfied` does: it keeps the diagnostic about *comparison* (naming
  the `T: Ord` bound to add for a type parameter, or the missing impl for a
  concrete type, with the unimported-impl / blocking-bound notes) rather than
  about a missing `cmp` method, and it stops an unrelated inherent `cmp` from
  silently qualifying. `TypeChecker.ord_trait` is captured in
  `tc_register_trait` from `<std>/cmp.dt`'s `@lang("ord")` marker (see "Lang
  items"), like `display_trait`. Since the prelude imports `std::cmp` into every
  program (see "Modules"), it is populated everywhere; the NULL guard is now
  defensive.

**`==`/`!=` are deliberately left out** and stay a structural primitive: `OP_EQ`
reads no static type, so equality is free, import-less and universal on any two
values of one type — routing it through an `Eq` trait would make the commonest
operation depend on a trait impl (and, unlike `Ord`, `==` is *not* one of the
prelude's lang items), handing coherence the power to take `[1,2] == [1,2]` away
from a program whose modules disagree. `Eq` waits for a concrete consumer that
needs *custom* equality.

### Arithmetic operators and `std::ops`

`+`/`-`/`*`/`/`/`%` and unary `-` are the same move one operator family over
(milestone 55). The split is identical: two numeric operands keep the opcode
(`OP_ADD` and friends, no import, no frame), and anything else asks a trait what
the operator means. The six traits — `Add`, `Sub`, `Mul`, `Div`, `Rem`, `Neg` —
live in `std::ops`, each declaring exactly one method named for the operation.

The compiler treats them as **one mechanism, not six**. `OpsTrait`
(`include/sema.h`) indexes `TypeChecker.ops_traits[]`, and `ops_trait_table`
(`src/sema.c`) is a row per operator holding the three strings that differ: the
`@lang` marker (which *is* the method name — the marker names the operation, not
the trait), the trait's spelling for a diagnostic asking for a bound, and the
operator as written. `ops_trait_for_token` maps the token to the row; unary `-`
names `OPS_NEG` directly.

**Where this rewrite differs from `Ord`'s is where the rewritten node ends.**
`a.cmp(b)` is an `Int` that the outer `< 0` still consumes, so that rewrite
*reshapes* the binary node and keeps it. `a.add(b)` is the whole answer — its
type is the operator's type — so `rewrite_ops_call` *replaces* the node:
`*expr = *call` after `resolve_method_call_typed` has typed the call against the
already-known receiver. The caller must not touch the old payload afterwards,
which is why `EXPR_BINARY` reads `binary->left`/`right` out before calling.
Either way an arithmetic operator has stopped existing by the time codegen runs,
so the opcodes, the VM and the image format are untouched.

Three details are the design:

- **The choice is made on the operands' *solved* types.** The arith branch
  `infer_find`s both before asking `type_is_numeric`, since a closure parameter
  typed by its hint arrives as an unknown; a still-unsolved operand is an
  inference failure ("cannot infer the type of this operand of '+'") rather than
  a missing impl, because nothing can be asked of a type that is not yet decided.
- **`ops_satisfied` gates the rewrite**, exactly as `ord_satisfied` and
  `display_satisfied` do, and for the same two reasons: the diagnostic stays
  about the operator (naming the `T: Add` bound, or the `where T.Item: Add` one
  level down, or the missing impl with its unimported-impl / blocking-bound
  notes) instead of degrading to "no method named 'add'", and an unrelated
  inherent `add` cannot silently become what `+` means.
- **The right operand picks the impl** (milestone 75). The five binary traits
  take `Rhs` with a default of `Self` (`fun add(self, other: Rhs) -> Self`);
  `Neg` has no right operand and no parameter. `ops_trait_ref` builds the
  reference the gate asks about — `ty_trait(trait, &rhs_ty, 1)`, so `v * 2.0`
  asks for `Mul<Float>` and `v * w` for `Mul<V2>` — and the *call* it rewrites
  to then reaches the matching impl through the receiver-side argument selection
  described under "Selection by argument type".

  What made this possible was noticing that the obstacle was misstated. The old
  note here said a generic trait's parameters are never *inferred* where the
  trait is named, so an operator could not select by its right operand. True,
  and beside the point: an operator has both operand types in hand, so it does
  not need to infer the argument — it writes it down. The default is what keeps
  the change free: `impl Add for Int` and `fun twice<T: Add>` still mean
  `Add<Int>` and `Add<T>`, so nothing in `std` moved.

  Two consequences. A still-unsolved *right* operand is now its own failure —
  it is what chooses, so there is nothing to choose with — reported separately
  from the left one's. And a mixed pair no impl heads is a selection failure
  naming the reference (`'Cents' does not implement 'Add<Int>'`) rather than an
  argument mismatch on the rewritten call, which was the old message's cost:
  it described the desugaring instead of the mistake.
- **The result is an associated type** (milestone 76). Each trait declares a
  `type Output` and the method returns `Self.Output`, so a dot product
  `V2 * V2 -> Float` and a reversed `impl Mul<V2> for Float` both have a
  spelling. The operator side of this cost nothing: `rewrite_ops_call` builds an
  `EXPR_METHOD_CALL` and returns whatever `resolve_method_call_typed` says, so
  changing the trait's return type changed the operator's result with no
  operator-specific code at all.

  What it cost was a bound. There is no default for an associated type, so
  `T: Add` says nothing about the result — `a + b` is the opaque `T.Output` —
  and a generic keeps its accumulator only by writing `T: Add<Output = T>`.
  The old note here said such a binding "discharges the projection to the type
  the *caller* named rather than to what the impl bound it to". Half right: it
  does discharge by rewriting, but the promise is checked against the impl at
  every instantiation (`check_bounds_satisfied`), which is the trust every bound
  already rests on. What was genuinely missing was the *projection*-subject form
  of it — `where Self.Item: Add<Output = Self.Item>`, which `std::iter`'s `sum`
  and `product` need — and that is what milestone 76 built.

Since `other: Rhs` takes `Self` by default (and, for `Neg`, `-> Self`) these are
still object-safety violations, so none of the six can be a `dyn`; they are
bounds, the position `Ord` is in.

### Lang items

A **lang item** is a std definition the compiler resolves for a construct that
never spells it: `"{v}"` reaches `Display`, `a < b` reaches `Ord`, a `{v:>8}`
spec reaches `pad_*`/`float`. The compiler holds each on a `TypeChecker` field
(`display_trait`, `ord_trait`, `fmt_pad_*`, `fmt_float`) and the std source
declares which definition fills it with a `@lang("…")` marker
(`AttrNode`, `AttrKind ATTR_LANG`) — `@lang("display")` on the trait,
`@lang("float")` stacked on `float`'s `@native`. The parser (`parse_decl`)
sorts a decl's attributes into a *body* attribute (`@native`/`@intrinsic`, on a
bodyless fun) and this *marker* (which keeps the definition's body), so the two
compose; a `@lang` on anything but a trait, enum, or top-level fun is a parse
error.

Capture is `tc_register_lang_trait`/`tc_register_lang_enum`/
`tc_register_lang_fun`, called from the matching register pass: read the
marker's key, dispatch it to the field. Two
properties replace what the old hard-coded `mod_is_std(m,"fmt") && name=="Display"`
matches gave:

- **Locality and refactor-safety.** The fact "this trait is `Display`" now lives
  on the trait, not in a far C `if` keyed on its spelling and module — a rename
  keeps working, and a typo in the *key* is a loud "unknown lang item" rather
  than a capture that silently never fires.
- **Honoured only inside the standard library, inert elsewhere.** A user's
  `@lang` cannot claim a lang item — so it cannot hijack what `"{v}"` dispatches
  to — but it is *not* an error, because a std file loaded by path
  (`ducktape std/fmt.dt`) carries the same marker yet is not the embedded
  `<std>/…` key, and "a std file is an ordinary module, directly runnable" is a
  property worth keeping (`mod_is_std_module` is the gate). The unknown-key error
  is therefore reachable only from within std, which is exactly where a typo
  would be. Milestone 77 makes the inertness matter for a *type* rather than a
  dispatch target, which is a sharper claim — a user's `@lang("option")` on its
  own two-variant enum would otherwise redirect what every `as?` in the program
  builds — so `tests/run/dyn_downcast_lang_inert.dt` pins it.

**`Option` is the one lang item that is a type** (`TypeChecker.option_enum`,
`@lang("option")` in `std/option.dt`, milestone 77), and it is worth saying why
it took until then. Every other consumer of `Option` takes one *apart* and can
do so structurally: `for` reads `next()`'s result through `enum_is_optionish`,
and `?` builds its early `Err` out of the operand's own `EnumDef`. A downcast
(`d as? T`) is the first construct that must *produce* an `Option` with no
operand to read the definition off — so it is the first that has to name the
type. The marker captures only the definition; which variants are `Some`/`None`
is still asked structurally at the use site, since the variants are stubs at
registration time.

### `for` over an iterator

`for x in it` handles an array and a range by their built-in shape. Anything
else routes through the `Iterator` lang item (`resolve_for_iterator`, the
`EXPR_FOR` case), in two moves that mirror two features already here:

- **Nominal gate.** `it`'s type must implement `Iterator`, asked through
  `impl_index_implements` — the same question `display_satisfied` /
  `ord_satisfied` ask — so the diagnostic is "does not implement 'Iterator'"
  (with the unimported-impl / blocking-bound notes) rather than a missing
  method. This is why the design is the *trait*, not a structural "has a
  `next()`": a bare `next` on an unrelated type does not make it iterable. A
  `dyn Iterator` is admitted directly — it *is* the trait rather than
  implementing it through an impl in the index — so the gate also accepts a
  `TY_DYN` whose trait def is the iterator trait, and codegen drives it through
  the vtable.
- **Structural unwrap.** The loop synthesises `it.next()` — an `EXPR_METHOD_CALL`
  resolved with `resolve_method_call_typed`, the receiver already typed, exactly
  the Display-interpolation desugar — and reads its result's shape with
  `enum_is_optionish`, the `Some`/`None` sibling of `?`'s `enum_is_resultish`.
  So the checker needs no handle on `Option` beyond the two variants of whatever
  `next` returns, and `Item` is that `Option`'s type argument. `Item` is run
  through `infer_apply` before it becomes the loop variable's type: an adapter
  whose `type Item = I.Item` yields a *projection* (`Filter<Counter>.Item` is
  `Counter.Item`), which is concrete but only resolves to the element type once
  read through the impl. The resolved call and the two variants are recorded on
  the `ExprFor` node for codegen (see runtime.md); an array/range loop leaves
  them NULL.

The split is deliberate: the gate is nominal (the trait is the contract), the
unwrap is structural (the stance `==` and `?` take on a std-shaped value).

### Types and inference

`Type` (`include/ast.h`) is a tagged union; structural types are interned so
identity is pointer equality (`types_equal`). The interning table (`src/ast.c`)
is open-addressed and doubles at 70% load — it has to grow rather than cap,
because a program's distinct-type count is bounded by nothing but the source,
and a fixed table could only ever abort on a valid program. Growth rehashes,
so it happens *before* a probe: a slot index is only meaningful under the mask
it was computed with, the same hazard as canonicalising a generic's bounds
after hashing it.

The table is a process global, because pointer identity means two `Type *`
compare equal only if one table produced them. Its entries — and the array —
are compiler-arena memory, so `type_intern_reset` runs in `compiler_destroy`
before that arena goes; nothing may hold a `Type *` across it. Singletons: Int, Float, Bool,
`Char` (`TY_CHAR`), String, `StringBuf` (`TY_STRBUF`), `()` (unit), `!`
(`TY_NEVER` — produced by blocks ending in `return`, unifies with anything),
`Range` (`TY_RANGE`, Int-only), and `TY_POISON`.

`TY_NEVER` is also writable, spelled `Never` in `TYNODE_NAMED` alongside the
other primitives. That one line is the whole of the language support for
`panic`: a signature can now *promise* divergence, and because unification
already let `!` stand in for any type, `return panic(msg)` satisfies any return
type with no coercion and no further rule (`language.md` "`std::panic`").

`Char` is written in `TYNODE_NAMED` the same way, and is the cheaper half of
the milestone that introduced it: adding a *primitive* to the checker is four
lines beside `Never`, a case in `resolve_expr`, an entry in the three inert
type switches, and its name in the interpolation primitive list
(`interp_render_bare`, the bare-segment half of `check_interpol_seg`) — which is
the one place it differs from `StringBuf` below, since a Char renders itself.
Everything else about the milestone is in the scanner, the value
representation, and the image format; nothing in the checker had to learn what
a character *is*.

`StringBuf` is written in `TYNODE_NAMED` the same way, and the checker knows
nothing else about it: it is inert in every switch it appears in (a singleton
to `subst_apply`, `infer_unify` and `infer_apply`) and is deliberately *absent*
from `interp_render_bare`'s primitive list, so `"{b}"` goes down the `Display`
path and reports that a buffer has no impl. That is the same arrangement
`String` already has — a builtin *type* whose operations live in std — so it is
not another lang item in the sense `Display` is (see "Interpolation and
`Display`"): the compiler knows the type, never a std item.

`Range` joined that list in milestone 60, and it is the cheapest entry yet: the
type had existed since ranges did, so the whole change is `t_range` on the
`TypeChecker` and a line in `type_named_builtin`. Everything it buys follows
from being *nameable* rather than from anything new — a range can be a
parameter (`fun span(r: Range)`), and `impl Range { .. }` has a self type to
write, which is what let `std::iter` hang `iter()` off one. Note the ordering
consequence a builtin name carries: `type_named_builtin` is consulted before
the type scope, so a program's own `struct Range` is declared but never
reachable by name — the same thing `struct String` has always done.

Inference is union-find over `TY_UNKNOWN` nodes: `infer_fresh` mints
unknowns, `infer_unify` solves (emitting "type mismatch" diags itself),
`infer_find`/`infer_apply` chase and deep-substitute solutions,
`infer_finalize` reports unsolved unknowns ("cannot infer type"). Generic
instantiation goes through `infer_open_generics`, which builds a `Subst`
(name → type) mapping type params to fresh unknowns or explicit args;
`subst_apply` rewrites types under it. Both of its arrays carry their own
length, and the *explicit-argument* one is the load-bearing count: a call with
no turbofish supplies zero arguments against however many type parameters the
definition has, so the two are routinely different and an argument slot exists
only where `i < type_arg_count`.

A generic constructor with no explicit type arguments normally opens them into
fresh unknowns and lets its *fields* solve them, which is why `Opt::Some(1)`
needs no turbofish. A constructor with no fields has nothing to solve them
from, so `hint_type_args` seeds them from the expected type instead: when the
hint names the same struct or enum, its type arguments are passed to
`infer_open_generics` as if they had been written out. Seeding only — a hint
that disagrees with a field is still unified and still reported at the site
where the value is used. This is what makes `Opt::None` work in every position
that supplies a hint, including an argument to a *non-generic* function, where
the parameter is compared with `types_equal` and an unsolved unknown would
never have been bound.

The hint's arguments may be type *parameters* rather than concrete types, and
that case needs no special handling — seeding with them is the identity
substitution when they are the constructor's own. It used to be excluded: a
`hint == self_type` test read "the hint is the bare abstract self type, so it
says nothing". Since `TY_GENERIC` interns by name, the `T` a function declares
and the `T` an enum declares are the *same node*, so inside `fun f<T>(xs:
[Opt<T>])` the hint `Opt<T>` is pointer-identical to `Opt`'s self type and the
test fired on the one case that had the answer. Dropping it is the whole fix;
the symptom read as "an argument hint will not reach the enclosing function's
parameters" when it was really "will not reach parameters that share a name",
which is why renaming one of them made it go away.

Seeding puts two distinct `TY_GENERIC`s in front of `infer_unify` where an
unknown used to absorb everything, and that exposed the one failure path in
that function that reported nothing: `case TY_GENERIC` returned a bare `false`.
A silent false becomes `t_poison`, which propagates silently *by design*, so
`Slot::Full(v, k)` against `Slot<K, V>` type-checked with no message at all.
Every failing branch of `infer_unify` now emits its mismatch, per the
convention the rest of the checker already follows.

Call arguments are checked **left to right, each hinted by what the earlier ones
already solved.** `resolve_call_expr` runs the parameter type through
`infer_apply` before handing it to the argument as a hint, rather than using the
raw `param_types[i]`. This matters when one argument's type mentions another's:
`map(it, |x| ...)` on `fun(I, fun(I.Item) -> B)` binds `I` from the first
argument, so the second's hint collapses the projection `I.Item` to the concrete
element type — and the closure body can be checked at all, since an abstract
`I.Item` is neither numeric nor comparable. Without the progressive apply the
projection would stay abstract and the closure could not resolve.

All three spellings of a variant constructor reach the same `EXPR_VARIANT`
case: `resolve_call_expr` and the struct-init path re-resolve after
`rewrite_tuple_variant_call` rewrites the node, and so does the bare
multi-segment `EXPR_PATH` (`Status::Off`) — returning the resolved path's type
directly there would hand back the enum's own abstract `Opt<T>`.

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
carrying `bounds`/`bound_count` — each bound a `TY_TRAIT` *reference*, so it
carries the trait's own type arguments (see "Generic traits").

`resolve_type_params` resolves the list and defines each parameter in the type
scope *as it goes*, which is what lets a bound name an earlier parameter
(`<T, U: Into<T>>`). Left to right, so a forward reference is an unknown type
rather than a second pass.

Enforcement is deferred to the end of inference, because a bound can only be
judged once its parameter is solved. `infer_open_generics` stashes the source
`TY_GENERIC` in each fresh unknown's `.bound` — rewritten through the
substitution it just built, so a bound mentioning a sibling parameter
(`U: Into<T>`) becomes `Into<?T>` and is checked against `T`'s own solution
rather than against a literal that no impl heads. That rewrite is the one
*partial* `subst_apply`: a bound can mention parameters of an enclosing scope
too, and only this declaration's are being opened. `check_bounds_satisfied`
skips a bound that is still abstract after `infer_apply`, on the same trust
`impl_index_implements` places in a `TY_GENERIC` — it is re-checked where the
enclosing definition is instantiated; after `infer_finalize`,
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

### Supertraits

`trait DoubleEnded: Iterator` is a bound on `Self`, so it resolves through
exactly the machinery a type parameter's bound does: `resolve_trait_supers`
hands the `TraitBound` to `resolve_bound_refs` and stores the resolved
references on `TraitDef.supers`. An associated-type *binding* is refused there
(a NULL `assoc` list) — it would be a promise about `Self`, which this
declaration does not bind.

**A supertrait is not a new kind of thing; it is an edge.** Four questions the
checker already asked of an abstract type each used to be answered by scanning
one trait's own lists, and each now scans the closure instead:

| question | reader | was | is |
| --- | --- | --- | --- |
| what does it implement? | `impl_index_implements` | `types_equal(bound, want)` | `trait_ref_entails` |
| what methods may it name? | `resolve_bound_method_call` | `def->methods` | `trait_flat_method` |
| what associated types has it? | `generic_assoc_owner`, `TYNODE_ASSOC`, `resolve_self_assoc_bounds`, `resolve_assoc_bindings` | `def->assoc_types` | `trait_flat_assoc_owner` |
| what can a vtable carry? | `trait_check_object_safe`, `cg_vtable_for` | `def->methods` | `trait_flat_method` |

`TraitDef.flat` is that closure: one `TraitFlat { def, ref }` per trait in it,
supers first in declaration order, depth-first, deduped by definition, this
trait last. Each entry's `ref` is stated in *this* trait's type parameters
(`trait Twice<T>: Src<T>` records `Src<T>` with `Twice`'s `T`), so a reader
restates it through `trait_ref_subst` of whatever reference it holds — the same
substitution a call through a bound has always applied to signatures, one level
out. A trait with no supers has exactly one entry, itself, which is what lets
each reader *drop* its special case rather than gain one.

Building it needs every trait's `supers` already resolved, and resolving those
needs every trait's *name* bound — so the two live in separate sweeps of the
supertrait sub-pass, and the associated-type names moved into the declare pass
(a straight copy off the AST) so `Self.Item` can reach a super declared later in
the file. `trait_build_closure` is recursive and memoised on `flat_built`, which
doubles as the "already on the stack" mark; `trait_super_cycle` is what
*reports* a cycle, since a walk that merely terminated would answer the four
questions above with whichever half it reached first.

The obligation half is `tc_check_impl_conformance`: each direct super,
substituted through the impl head's trait reference, must be satisfied by the
impl's self type in the impl's *own module*'s visible set. That check is the
only thing making the assumption half sound — `impl_index_implements` reads a
super off a `TY_GENERIC`'s declared bounds without looking for an impl at all.

#### Deriving the super's impl

The obligation is *discharged* before it is checked. `derive_impl_supers` walks
one impl's closure and, for each super the module does not already implement,
synthesises a real `ImplDef` — same module, same self type, the same
`type_params` array (so the derived impl applies under exactly the conditions
the written one does), the super's reference as `trait_type`, and the block's
own `MethodDef`s for whatever items the super declares. `impl_lacks_super_item`
decides derivability and is asked twice: here, and by
`tc_check_impl_conformance` to *name* the item that stopped it.

Three things make this small rather than a second kind of impl:

- **The `MethodDef`s are shared, not copied.** One `FunDef` per body, so
  nothing is compiled twice and a name cannot resolve two ways. The derived
  impl simply lists the subset the super declares; a super method the block
  omits is one the super defaults, and `impl_index_default_method` finds a
  default through a derived impl exactly as through a written one.
- **Nothing had to be taught that these methods were legal in that block.**
  `method_is_inherent` already asks `trait_flat_method_index` — the closure —
  so a super's method written in the sub's block was never an inherent name,
  and `impl_defs_share_method` skipped it. The block always accepted the items;
  only the obligation loop rejected the impl.
- **It is a fourth sweep of `tc_resolve_module`, not a step of
  `resolve_impl_decl`.** The obligation is discharged against what the *module*
  implements rather than against what precedes the impl in the file, so
  deriving in place would make `impl Loud for P` written above an explicit
  `impl Greet for P` derive one and then collide with it. Every dependency's
  impls arrived in `tc_import_impls`, one phase earlier.

A written impl always wins, so derivation can never create a conflicting pair.
The cost of that rule is `report_super_item_taken`: where the super is already
implemented and the block writes one of its items anyway, the copy in the block
is unreachable through the super while a receiver call finds it and not the
other — milestone 68's argument one relation over, so it is an error where the
second definition is written.

Two details are consequences rather than choices:

- **`trait_project` has to look past the impl it was given.** A supertrait's
  associated type is bound by the impl of the trait that *declares* it, so
  `impl DoubleEnded for Rev<I>` says nothing about `Item`. When the impl does
  not carry the name, the binding is looked up on its self type through
  `impl_index_assoc_type`, against the impl's own module.
- **`type_mentions_self` asks the table, not the trait.** `Self.Item` is not a
  `Self` *mention* precisely because a `dyn` pins it; with supertraits the
  pinning table is the flattened one, so the test is
  `trait_flat_assoc_index(trait, name) >= 0`.

### Generic traits

A trait's type parameters are ordinary generic parameters of every signature it
declares. `TY_TRAIT` is therefore a *reference* — a `TraitDef` plus type
arguments, interned on both — and a reference is written at each of the three
places a trait can be named: an impl head (`ImplDef.trait_type`), a bound
(`TypeGeneric.bounds`), and inside a `dyn` (`TypeDyn.trait`). `trait_ref_resolve`
builds one and is where the arity is checked; a *bare* trait name in type
position resolves to `TraitDef.self_type`, the trait applied to its own
parameters, which is a declaration's meaning of it and reported as a missing
argument anywhere else.

`trait_ref_subst` turns a reference into the substitution its signatures are
read under, and three places apply it: `tc_check_impl_conformance` (before
comparing an impl's method against the trait's), `check_trait_method_call` (a
call through a bound), and `cg_trait_ref_subst` (instantiating a default body).
Each drops entries a method's own type parameters shadow, exactly as every
other outer substitution is dropped. Nothing else in the pipeline changed:
`subst_apply`/`infer_apply`/`infer_unify` walk a reference's arguments like a
struct's, and the backend never sees one at all.

Selection asks about the pair. `impl_applies` takes a nullable `trait_ref`
alongside the self type, and matches the impl's head against *both* — so an
impl's own parameters may be pinned by either half (`impl<T> Into<T> for
Wrap<T>` by the receiver, `impl<T: Display> Into<String> for T` by both), and
`impl Into<Int> for S` no longer answers `S: Into<String>`. Coherence
(`impl_defs_conflict`) asks the same question from both sides, which is what
lets one type implement one trait at several arguments.

Two consequences worth naming, because they are the only places the checker
had to decide something new:

- **A bare method call may have several bodies.** The receiver alone cannot
  pick between `Into<Int>` and `Into<Fahrenheit>` for one type, and a bound
  would have named the reference. So `impl_index_method` takes a `ret_hint` —
  the expected type, threaded from `resolve_method_call_expr` — and uses it as
  a *tie-break* only: with one candidate it is never consulted, so a wrong
  hint still reports the ordinary mismatch rather than "no method".
- **A trait argument can be solved rather than checked.** `[dyn Into<T>]` at a
  call site names no argument, so `check_coerce_dyn` reads it off the impl
  (`impl_index_trait_ref`) and unifies — the same exception milestone 27 made
  for an unsolved associated-type binding, one bracket list over. Unlike a
  binding this one can be ambiguous, since a type may implement the trait at
  two arguments, and that is reported rather than guessed. The recorded
  `Expr.coerce_dyn` is therefore queued like `inst` and rewritten by
  `cctx_solve_insts` once inference settles.

#### Default type parameters, and a bound that names its own subject

Milestone 75 let a trait's parameter carry a default (`trait Add<Rhs = Self>`),
which is what made the operator traits heterogeneous without moving a line of
`std`. Two pieces, and the second is the interesting one.

**The default is stored unresolved.** `TraitDef.type_param_defaults` holds
`TypeNode *`s parallel to `type_params`, because `Self` in a default means *the
type the trait is being applied to* — there is no one type to resolve it to
once. `trait_ref_resolve` gained a `subject` parameter: it fills the tail of the
argument list from the defaults, resolving each in a scope where `Self` is
`subject`. Passing NULL leaves `Self` to the ambient scope, which is right
exactly where the position already defines it — an impl head, a trait's own body
— and gives "unknown type: Self" where nothing does. `resolve_bound_refs` threads
the subject: the parameter for an inline or `where T:` bound, the projection for
a `where T.Item:` one, the trait's `Self` for a supertrait. The defaults must be
a suffix of the list, checked once at `declare_trait_decl`, which keeps
`trait_ref_resolve` a count. And each default node's `resolved` cache is cleared
after use, since one written default is read against many subjects.

**At a bound the subject is the parameter itself**, so `T: Add` denotes
`Add<T>` — a bound naming its own subject, which the language could not
previously say (`fun f<T: Scale<T>>` was "unknown type: T"). It reads like a
cycle rather than a scoping bug: a `TY_GENERIC` is interned on its *bounds*, so
`T` bounded by `Add<T>` is a type that contains itself.

It is not a cycle, and the reason is one line of `subst_apply`: **a `TY_GENERIC`
is matched by name.** `resolve_type_params` defines each parameter in scope
bound-less *before* resolving its own bounds, so the occurrence inside the bound
is a placeholder — a second interned node with the same name and no bounds. The
representation stratifies instead of looping, and because a substitution binds
the *name* `T`, both nodes rewrite together at every instantiation. The
stratification is therefore invisible to the whole pipeline except in one place:
inside the declaration's own body, where the parameter stays abstract and
pointer identity is the entire test.

Three readers collapse the two spellings again, all through `bounds_rebound`,
which applies the one-entry substitution `T := T`:

- `generic_bounds_rebound` — `resolve_method_call_typed`, so `v + v` inside
  `fun twice<T: Add>` sees `add`'s parameter as the bounded `T`.
- the same, in `impl_index_implements`, so asking whether `T` satisfies `Add<T>`
  compares like with like.
- `assoc_bounds_rebound` — the projection form, for the bounds a
  `where T.Item: Add` put on `T.Item`. A trait method's `where Self.Item: Add`
  is the same shape one level up, which is why the subject recorded there is a
  projection off `trait_self_param(trait, NULL, 0)` — the bounds-only `Self`,
  because the real one carries the very assoc bounds the loop is still building.

`ty_assoc` handles the equality-binding half directly: `T: Scale<Out = T>`
discharges `T.Out` to the placeholder, so the constructor hands back the `base`
it was given whenever the bound-to type is a generic of the same name.

This is a general language feature, not a private spelling — `T: Add<T>` and
`where T: Scale<T, Out = T>` are writable (`tests/run/self_referential_bound.dt`).

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

**The same dispatch, with no receiver.** A trait may declare an *associated
function* — a signature with no `self` — and then there is nothing to dispatch
on, so the path says it instead: `T::make(1)`, or `Self::make(1)` inside a
default body. `resolve_path` grew a context for a type parameter
(`PATHRES_CTX_GENERIC`), which searches the parameter's bounds exactly as
`resolve_bound_method_call` searches a receiver's, and returns
`PATHRES_TRAIT_ITEM`. `check_trait_item_path` then does what
`check_trait_method_call` does, minus the receiver: open the method's own type
params, apply the trait reference's arguments, `trait_project` the signature,
record `Self` in `inst`, and set `bound_trait`/`bound_self` — on the
`ExprPath` this time. Codegen's two branches share `cg_bound_target`.

A `self` parameter is *not* special there: the signature is used whole, so
`T::value(v)` passes the receiver as an ordinary first argument. A method is
an associated function whose first parameter is `self`, and the path form is
the spelling where that is visible — which is why one branch serves both.

A **builtin type** may qualify a path the same way (`Float::from(7)`). The
struct path context carried only a `Type` anyway, so it became
`PATHRES_CTX_TYPE` and the builtin names moved into `type_named_builtin`,
shared with `TYNODE_NAMED` so the two spellings of `Float` cannot drift.

An **enum** qualifies one too, and is the one context where a segment has two
readings: `PATHRES_CTX_ENUM` asks `find_enum_variant` first — the enum's own
declaration — and falls through to the impl index when nothing answers. The
impl half is `pathres_assoc_item`, shared verbatim with `PATHRES_CTX_TYPE`; it
reports everything except the miss, which goes back to the caller, since only
the caller knows whether "no such associated item" is the whole story (a
struct) or half of it (an enum, whose message names both). The fallback is
unambiguous rather than a precedence rule: `impl_shadows_enum_variant` refuses
an inherent method under a variant's name where the impl is written, so the
two readings can never both apply.

### Trait default bodies

A default body is checked and compiled *once per receiver type*, like the
generic function it effectively is:

```
trait Show { fun twice(self) -> Int { self.show() + self.show() } }
⇒            fun twice<Self: Show>(self: Self) -> Int { ... }
```

`resolve_trait_default_impl` builds that `FunDef` at resolve time: its type
params are `Self` — `ty_generic("Self", bounds=[the trait's own reference])` —
then the trait's type parameters (those the method does not shadow), then the
method's own, and its signature is `method_type` run through `trait_project`
onto that parameter. The trait's `method_type` itself keeps the abstract
`TY_TRAIT` `Self`, since impl conformance and call sites are checked against
it; only the *body* needs a `Self` that a `Subst` — which is keyed by name —
can bind, which is what the milestone turned on.

Everything downstream then falls out of machinery that already existed:
`tc_check_trait` checks the body with `Self` in scope as that type parameter,
so `self.other()` inside it is an ordinary call through a bound, and codegen
instantiates it like any other generic definition.

`tc_check_trait` builds that scope from `fun->type_params` **by name**, since
that list is the body's actual signature. Reading it positionally off the
*item* instead — as it did until milestone 53 — silently binds a method's own
`<U>` to the trait's `<T>` for any generic trait, and the instantiation then
has no `U` to bind; it also left the trait's parameters out of scope entirely,
so a body could return a `T` but not name one in an annotation.

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
`subst_apply` substitutes inside the base but by default does not collapse the
projection; `infer_apply` does, reading the binding off the applicable impl as
soon as the base is concrete, and `infer_unify` normalises `TY_ASSOC` operands
up front so `T.Item` and `Int` don't look like different kinds. While the base
is still abstract the projection survives unchanged, which is exactly what a
generic body needs. A projection on a *trait object* never survives that long:
`trait_project` reads it off the binding the `dyn` names, since a trait object
is the one abstract receiver that says what its associated types are.

**A projection can be bounded, and the bound lives on the base.** `where
I.Item: Ord` is stored as an `AssocBound` (name, trait refs, and — for the
equality kind below — the type it must *be*) on the
*parameter's* `TY_GENERIC`, not on the `TY_ASSOC` it constrains — `TY_ASSOC`
stays a plain (base, name) pair and a projection is answered by looking through
its base. That placement follows from where the bound has to be *discharged*:
binding `I` to `Counter` is the moment `Counter.Item: Ord` becomes a question
with an answer, and that moment is keyed by the parameter. It also keeps the
projection's identity independent of what happens to be known about it.

Three sites read it, mirroring the three a plain bound already has.
`impl_index_implements` gains a `TY_ASSOC` case that answers from the base's
declared list, exactly as its `TY_GENERIC` case answers for a parameter — so
`v > b` on two `I.Item`s resolves, and a bounded projection satisfies another
definition's bound without being made concrete first. `resolve_method_call_typed`
offers the same list as the receiver's bounds, so a method call on a projection
dispatches. And `check_bounds_satisfied` discharges it: project the concrete
argument through `impl_index_assoc_type`, then ask the ordinary question.
Soundness is the same argument a bounded parameter already rests on — the
abstract answer is trusted inside the body because the concrete one is checked
at every instantiation.

Because a bound is part of what a parameter *is*, `ty_generic` interns on the
associated-type list too, canonicalising it (by name and owning trait, then each
entry's traits) the way it already sorts plain bounds: `I` and
`I where I.Item: Ord` accept different instantiations and must not share a type.

**An entry is keyed by the trait as well as the name** (milestone 76). While
`Item` was the only associated type in the language a name identified an entry,
and both the bound list and `impl_index_assoc_type` searched by name alone.
Giving all six `std::ops` traits an `Output` broke that in two places at once:
`T: Add<Output = T> + Mul<Output = Int>` merged into one entry and reported the
second binding as contradicting the first, and a `Cents` with two impls binding
`Output` answered a projection with whichever impl was registered first. So
`AssocBound` carries the declaring `owner`, the entry lookup and every reader
match on it, and `impl_index_assoc_type` takes it as a filter — NULL only where
the caller genuinely has no trait in hand, which is a projection off a concrete
base written in source (`V2.Output`), the one spelling with no syntax for
naming a trait.

**An equality binding is a rewrite, not a check.** `J: Iterator<Item = I.Item>`
is the second predicate kind, stored as `AssocBound.equals` beside the trait
list — the same entry, since both constrain one projection. What it says is not
"satisfies" but "is", and that difference is the whole design: `ty_assoc` reads
the base parameter's list at *construction*, so building `J.Item` returns
`I.Item` and the losing spelling never exists. Unification, `subst_apply`,
`infer_apply`, monomorphisation and the VM therefore need nothing — there are
not two types to reconcile. (One hop suffices and cannot cycle: the stored type
came through the same constructor, and a bound may only name a parameter
declared *earlier* — or, since milestone 75, the subject itself, which
`ty_assoc` recognises by *name* and answers with the spelling the caller
already holds.)

**The subject may itself be a projection** (milestone 76). `where Self.Item:
Add<Output = Self.Item>` is the shape a reduce over a bounded element type
needs, and it had been rejected outright — a binding is recorded on the thing
being bounded, and a projection had nowhere to record one. It does now:
`AssocBound` carries a nested list of its own, `resolve_bound_refs` is handed
that list as the sink when the where-lhs names a projection, and `ty_assoc`
gains the matching second collapse (walk to the `TY_GENERIC` base, find the
outer entry, find the nested one). Depth stops at two, because only a trait
ref's bindings build a nested entry and only a where-lhs — capped at one
associated type — builds a level for them to sit on.

Two things the nesting forced. Every rewriter of an associated-type bound list
has to reach the nested level or an instantiation would compare a promise still
spelled in the definition's own terms; the substitution and the `Self`
projection are the two, so the walk is written once (`assoc_bounds_map`) with
the differing steps passed in. And `trait_self_param` now *projects* the list it
is given: the clause was resolved with `Self` in scope as the trait type, so a
binding spells its subject `Iterator.Item` while the default body — checked
against the `TY_GENERIC` `Self` being built — spells the same thing `Self.Item`.
Two spellings is exactly what an equality binding cannot survive, since
`ty_assoc` hands back what the binding named.

The price of a rewrite is that it must be earned, so the promise is discharged
in the two places a parameter is bound. `check_bounds_satisfied` compares the
argument's binding against the required type once inference settles — with
`infer_open_generics` first rewriting `equals` through the substitution being
built, since the type it names is usually a *sibling* parameter the same call is
solving. And `impl_bounds_satisfied` asks at impl selection, which for this
predicate kind is not a nicety: an `impl<I, J: Iterator<Item = I.Item>> Iterator
for Chain<I, J>` that applied to a `Chain<Counter, Words>` would compile
`Words`'s `String`s as the `Int`s the impl bound `Item` to. (Selection had never
looked at associated-type bounds at all, so `where I.Item: Ord` on an impl was
unchecked there too.)

Two smaller consequences are worth stating. A trait method's type parameter has
its bounds written in the trait's terms — `chain<J: Iterator<Item = Self.Item>>`
— so `check_trait_method_call` projects the *declarations* onto the receiver
(`trait_project_param`) before opening them, otherwise the discharge would ask
about the abstract `Self`. That projection needs its own reading of a bound: a
`TY_TRAIT` naming the trait means `Self` in a type position but the trait itself
in a bound position, so `trait_project_bound` moves only the arguments. And
since the rewrite leaves one spelling, a `where J.Item: Ord` written alongside a
binding would constrain a type nothing builds — refused, naming the spelling to
use.

**Discharging it against an argument that is itself abstract.** When
`check_bounds_satisfied` projects the argument and `impl_index_assoc_type`
finds no impl, the argument may be *another definition's* type parameter — a
generic handing its `J` to a generic wanting `where I.Item: Ord`. There is
nothing concrete to consult, so the question is asked one level up: build
`J.Item` and hand it to `impl_index_implements`, whose `TY_ASSOC` case answers
from what *that* declaration promised. Without this the assoc half simply gave
up where the plain half (whose `TY_GENERIC` case has always answered this way)
did not, so a bound could be dropped in transit and the mistake surfaced as a
codegen error inside the callee at some later instantiation.

#### On a trait method's signature

`where Self.Item: Ord` is the same predicate in the one position where the
thing it constrains is not a parameter. `Self` in a signature is the abstract
`TY_TRAIT` — there is no `TY_GENERIC` to hang an `AssocBound` on, and a
required method has no default body to borrow one from — so the list is stored
twice, for two readers with different jobs:

- **the obligation**, `TraitMethodDef.self_assoc_bounds`, resolved by
  `resolve_self_assoc_bounds` from the same `where` clause the method's own
  type parameters already consumed. `method_type` keeps the abstract `Self`,
  because conformance and call sites check against the trait rather than
  against the body copy;
- **the assumption**, copied onto `trait_self_param`'s `TY_GENERIC` `Self`, so
  the default body reads it exactly as a `<I: Iterator>` function reads its
  own — `v > b` on two `Self.Item`s resolves through the machinery above with
  nothing added. `Self` is therefore per *method*; interning makes it the same
  node for every method that declares no such bound.

The discharge site is what differs from a parameter's bound. A parameter is
bound in one place, so `infer_check_bounds` can wait for inference and ask
there. `Self` is decided by whichever receiver a call has, so
`check_trait_method_call` records the obligation — a `TY_GENERIC` carrying just
these bounds, paired with the receiver, pushed onto the same
`InferCtx.explicit_bounds` list an explicit turbofish argument uses — and
`check_bounds_satisfied` answers it after `infer_finalize`, since the receiver
may still be an unsolved unknown when the call is checked (`xs.iter().max()`).
Concrete receiver, abstract receiver and the relay case above are then one code
path, not three.

The fourth object-safety condition falls out of the same asymmetry: a method
with such a clause is `undispatchable`, because a vtable is built where the
value is *coerced* and whether the bound holds is a question about the concrete
type and the impls visible there.

**Collapsing is a capability of the traversal, not of the caller.** What
`subst_apply` lacks is not the ability but the *index* — the binding lives on an
impl, and looking it up needs an `ImplIndex`. So `subst_apply_` takes one as an
optional argument and its `TY_ASSOC` case uses it when present, calling
`impl_index_assoc_type` and **recursing on the answer**: a pass-through adapter
binds `Item` to a projection of its own, so `Filter<Counter>.Item` →
`Counter.Item` → `Int` needs more than one hop. `subst_apply` passes NULL — every
checker caller wants the projection left standing, because `infer_apply` will
finish it against the inference substitution — and `assoc_apply(impls, t, al)`
is the same traversal with an empty substitution and an index, which is how
**codegen** gets the collapse without an `InferCtx` (see `runtime.md`,
"Monomorphisation"). Two entry points, one walk; the difference between them is
one pointer.

### Trait objects (`dyn Trait`)

Two type kinds, deliberately not one. `TY_TRAIT` is the *abstract* `Self` of
a trait — bound position, default bodies — and is resolved statically.
`TY_DYN` is a trait object: a real runtime representation, and so `concrete`
as far as codegen is concerned (`type_is_concrete` says yes, and a `dyn Show`
can key an instantiation exactly like a struct can). Sharing one kind would
put two dispatch strategies behind one type.

Resolution is `TYNODE_DYN` → `ty_dyn(trait_ref, bindings)`, interned like every
other structural type. The bracket list after the path holds two lists —
positional type arguments for the trait itself, then named associated-type
bindings — told apart by two tokens of lookahead in `parse_dyn_args`, since a
type argument may be a bare name too. Arguments first, because the positional
half cannot survive being interleaved. **Object safety** is checked there — where `dyn Trait`
is written, not at the trait declaration, because a trait is free to be
static-dispatch-only and only naming it as a type asks for more. A method is
vtable-able only if a receiver and nothing else is enough to call it:
`self` required, no method-level type parameters, and `Self` nowhere but the
receiver (`type_mentions_self` looks past the receiver position).
`trait_method_undispatchable` is that single test, run once when the signature
resolves and cached on `TraitMethodDef.undispatchable`.

**Object safety is partitioned per method, not all-or-nothing.** A
non-dispatchable method sinks the trait only if it is *required*; a *provided*
one (a default body) is instead **excluded from the vtable** — codegen skips its
slot, leaving `NULL`, round-tripped through the image as a sentinel index. So a
trait may carry generic convenience methods and stay object-safe: `Iterator`'s
`map`/`filter` are excluded (a type parameter, a `Self`-shaped return) while
`collect` — dispatchable, its `[Self.Item]` return a projection the object still
names — stays in the vtable, which is exactly what lets the combinators be
methods without taking `dyn Iterator` away.

As `std::iter` filled out, that partition turned out to fall along a line the
library already had: **an adapter returns a `Self`-shaped type and a consumer
does not**, so every adapter is excluded and a consumer is dispatchable unless
it has a type parameter of its own (`fold`) or a bound on the element (`max`).
The rule was never written for that split; it is what the dispatchability test
happens to mean once applied to the two shapes.

**The partition governs the vtable, not what a program may call** (milestone
56). An excluded method used to be rejected at the call, because the vtable was
the only way in. It is not: a provided method is a generic function over `Self`
(`TraitMethodDef.default_impl`, see "Trait default bodies"), so
`compile_method_call` instantiates its body at `Self = dyn Trait` instead of
emitting `OP_DYN_METHOD` into the empty slot, and the `self.next()` calls inside
it dispatch through the table on their own. `d.map(f)` therefore runs. The
`NULL` slot is still never read — codegen branches on `undispatchable` before
reaching for it — but the branch is now a choice of strategy rather than a
guard behind a checker rejection.

Two consequences worth stating plainly. A method excluded for
`self_assoc_bound_count` (`max`, carrying `where Self.Item: Ord`) has its
predicate *discharged* at the instantiation rather than waived: the call site can
read `Item` off the trait object's own type and ask the question the coercion
could not. And a concrete impl that *overrides* an excluded provided method is
bypassed — nothing vtable-less can see it, so `dyn Iterator`'s `map` is always
`Iterator::map`. Rust makes the same trade for the same reason.

**`Self.Item` is exempt, and that exemption is what `TypeDyn.assoc_types`
buys.** `Self` cannot be recovered once a value is coerced — that is what the
coercion erased — but a projection is not the erased type, it is a *function*
of it, and a function of an erased thing can be pinned by writing down its
result. So a `TY_DYN` carries one type per associated type in the trait's
**supertrait closure**, in that closure's order: the same table an `ImplDef`
carries, written at the use site instead. Total plus canonically ordered is what
lets interning keep deciding identity by pointer, and filling it *is* the
completeness check — a hole is a missing binding, a name matching no hole is a
wrong one. Over the closure rather than the trait's own list because a sub's
signatures project through a super (`dyn DoubleEnded<Item = Int>` binds
`Iterator`'s `Item`); `ty_dyn_assoc` and `impl_index_assoc_type` read it back
through `trait_flat_assoc_index`, so the layout has one definition. The
bindings are ordinary types, so `subst_apply`, `infer_apply`,
`type_mentions_self`, `trait_project` and codegen's `type_is_concrete` all
walk them the way they walk a struct's type arguments.

Dispatch reads the binding through `trait_project`: with a `TY_DYN` self, a
`Self.Item` in the method signature collapses to what the trait object says it
is, rather than being rebased onto a self that has no impl to consult (the
bounded-`T` case, which stays abstract as `T.Item`). So a projection through a
trait object reaches codegen *already* concrete, where one through a bound
arrives as a `T.Item` that codegen has to collapse itself — which since
milestone 50 it does, so both can key an instantiation; the difference is only
where the collapse happens.

Dispatch needs nothing new: `resolve_method_call_expr` treats a `TY_DYN`
receiver as offering exactly the one trait it names, and hands it to
`resolve_bound_method_call` like any other abstract receiver. Object safety
is what guarantees the projected signature stays callable once `Self` is
gone. Only *codegen* tells the two apart, by the vtable.

**A `dyn Sub` carries its supers, and needs no new rule to.** The vtable is
laid out over the closure (`trait_flat_method_count` slots, supers' first), and
`impl_index_implements` lets a `TY_DYN` witness anything the closure names — so
`d.next()` on a `dyn DoubleEnded` dispatches through the same table
`d.next_back()` does, and the value satisfies an `Iterator` bound. There is no
diamond problem to solve, which is why one flat numbering is enough: a trait
object always carries the table of the trait it was *written* as, and every
dispatch on it indexes that same trait's closure (`trait_flat_method_index` in
`compile_method_call` and `compile_for_iter`). Two tables never have to agree,
because the language has no `dyn A` → `dyn B` coercion.

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

Implementing the trait is not the whole question once the type carries
bindings: each is compared against what `impl_index_assoc_type` reads off the
applicable impl, and a disagreement is reported *here* rather than left to the
caller, whose mismatch could only say the two types differ and not that the
trait is implemented and only the binding is wrong. The one exception is a
binding that is still an unsolved unknown (`[dyn Iterator<Item = T>]`): the
impl is the only thing that knows, so that case unifies instead of comparing —
the single place the coercion solves rather than checks. That loop is
`dyn_assoc_bindings_agree`, shared with the downcast below: the two directions
ask the same question about the same pair and differ only in what they do with
the answer.

The result is recorded on the expression (`Expr.coerce_dyn`) rather than
folded into its type, so the node keeps saying what it *is*. That is the same
collect-then-consume shape `inst` uses for monomorphisation, and for the same
reason: at the moment the coercion is discovered the type may still be an
unsolved unknown, and codegen is the consumer either way.

**A trait object satisfies its own bound** (milestone 56), which is the sentence
above — "a bound whose witness travels with the value" — read in the other
direction. `impl_index_implements` already answers in three registers: a
concrete type searches the impl index, a `TY_GENERIC` reads the parameter's own
declared bounds, a `T.Assoc` over one reads its `where` clauses. A `TY_DYN` is
the fourth and the most direct — it has no impl and was promised nothing,
because it *is* the trait — so the answer is `types_equal(dyn.trait,
trait_ref)`. Trait references are interned, so that covers the trait's type
arguments; the *associated* bindings are not part of the question, because they
are part of the type.

`impl_index_assoc_type` is the other half: for a `TY_DYN` it reads the binding
off the value's own type rather than searching an index the coercion left
nothing in. Because `subst_apply` and `infer_apply` both project through that
one function, this is what collapses `I.Item` to `Int` everywhere at once — the
loop variable of a `for` over a bounded parameter, the element type of
`collect`, and the `where I.Item: Ord` a combinator carries.

**The downcast is that coercion asked backwards** (`resolve_downcast_expr`,
milestone 77): `d as? T` is `Option<T>`, and what makes it expressible is that
the *vtable* is already a runtime type identity — `Mono.vtables` memoises on
exactly (trait reference, interned self type), so codegen can ask for the table
`T` would have been coerced through and the VM compares pointers. See
`runtime.md`. The checker's whole job is therefore to establish that the pair is
one a coercion could have produced, which is the same two questions
`check_coerce_dyn` asks in the other direction — `impl_index_implements`, then
`dyn_assoc_bindings_agree` — plus a target that is not another `dyn` (that would
be an upcast, a different operation). Failing either is an *error* rather than a
statically-`None` test, since a type with no table is one the object could never
be holding.

The target's *abstract* forms are where the two lists part company. A coercion
declines a `TY_ASSOC` source; a downcast accepts one as a target, because
`d as? I.Item` under a `where I.Item: Shape` collapses at the instantiation
exactly as a `TY_GENERIC` does — the bound answers the checker, `cg_subst`
answers codegen. `Self` inside a default body works for the same reason, and is
the mirror of a standing gap: `self` there cannot be *made* into a trait object,
but a trait object can be recognised as it.

The node keeps three things for codegen: the trait reference (the half of the
vtable key the target does not supply), the resolved target, and the `Option`
lang item's two variants. Only the reference is queued through
`cctx_record_coercion`, for the same reason `coerce_dyn` is — a generic trait's
argument can still be unsolved. The target cannot need it: it comes from a
written `TypeNode`, and the grammar has no hole, so it never resolves to an
inference variable. Both still go through `cg_subst` at the instantiation, which
is the *other* rewrite — type parameters, not unknowns.

Two special cases died with it. `resolve_for_iterator` had carried a second
disjunct admitting a `dyn Iterator` by trait-def identity (milestone 47), since
the index was the only thing it asked; it now gates on the one question. And
`check_trait_method_call`'s rejection of an excluded method through a `TY_DYN`
receiver went too — keeping it would have made `d.map(f)` an error while
`f(d)`, handing the same value to a `<I: Iterator>` function that calls
`it.map(f)`, compiled the very same body. What is still refused there is an
*associated function*: no receiver means no value to carry, which is the one
undispatchable shape no instantiation rescues.

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
the runtime slot lives on the `FunDef` (assigned program-wide by codegen)
and travels with the copied `ve->as`, which is also what codegen reads back
off `ExprPath.resolved_fun` — a name may be an alias for a function in
another module.
Path resolution (`resolve_path` / `cctx_resolve_path`) walks segments through
type scope contexts (struct → associated fn, enum → variant, and a `use`d module
→ its export — see "Module-qualified paths"). In
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

`ImplIndex` is a flat list. `impl_index_method` matches a receiver type — and,
when the caller has one, a trait reference — against each impl's head: exact
`types_equal` for non-generic impls, `impl_type_match` (structural, binds the
impl's `TY_GENERIC` leaves) for generic ones. First match wins, except that a
`ret_hint` breaks a tie between two impls of one generic trait (see "Generic
traits"). "First match wins" is only ever *reached* for a trait-declared name:
inherent coherence rejects the pair that would otherwise make it arbitrary. A
*bare generic self* — the canonical `Point<T>` that a path
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
