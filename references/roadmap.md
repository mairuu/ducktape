# ducktape — roadmap

## Done

Milestones **through 54** are in `history/done-through-m54.md` and **55–75** in
`history/done-m55-m75.md`, split out to keep this file small. Everything from
76 on is below.

- **76. An operator's result is its own type: the wart's remainder was two
  questions, not one.** `std::ops` grows an `Output` associated type, so a dot
  product `V2 * V2 -> float` and a reversed `impl Mul<V2> for float` have
  spellings that `-> Self` could never give them. What the milestone actually
  built was a *binding whose subject is a projection*
  (`where Self.Item: Add<Output = Self.Item>`), which is what `std::iter`'s
  `sum` and `product` need and what had been rejected outright.
  - **The operator half cost nothing at all.** `rewrite_ops_call` builds an
    `EXPR_METHOD_CALL` and returns whatever `resolve_method_call_typed` says, so
    changing the trait's return type from `Self` to `Self.Output` changed every
    operator's result with no operator-specific code touched. The milestone's
    whole cost was in what a *bound* can say.
  - **THE FINDING: a type parameter may carry a default, an associated type may
    not — and that is the whole difference between the two halves of the same
    trait.** Milestone 75's `Rhs = Self` made its migration *zero*; `Output` has
    no such escape, so every impl states it and every generic that wants to keep
    its accumulator names it (`T: Add<Output = T>`). The old note predicted this
    would "break every generic use before it enabled one". Half right: every
    generic use did move, but there were exactly two in `std`, and the move is
    one clause each. What it got *wrong* was the reason — it read the binding's
    discharge as pinning the result to a caller's promise, when the promise is
    checked against the impl at every instantiation, which is the trust every
    bound already rests on.
  - **The blocker was where a binding could be recorded, not what it meant.** A
    binding hangs off the thing being bounded, and a projection had nowhere to
    hang one — `resolve_generic_param` passed `assoc = NULL` for a two-segment
    where-lhs and the diagnostic said so. So `AssocBound` grew a nested list of
    its own, and `ty_assoc` a matching second collapse. Depth stops at two by
    construction: only a trait ref's bindings build a nested entry, and only a
    where-lhs — already capped at one associated type — builds a level for them.
  - **The self-reference is recognised by name, one level down.** The type in
    `equals` is built against the parameter's bound-less placeholder, so
    comparing pointers would miss; two names decide it, the parameter's and the
    projection's. That is milestone 75's finding applied one level lower, and it
    is why `Self.Item.Output` collapses back to `Self.Item` rather than to a
    second interned node spelling the same thing.
  - **`Output` is the first associated-type name two traits share, and it broke
    two lookups that a unique `Item` had been hiding.** The bound list keyed
    entries by name, so `T: Add<Output = T> + Mul<Output = int>` merged into one
    and reported the second binding as contradicting the first; and
    `impl_index_assoc_type` searched impls by name, so a `Cents` with two impls
    binding `Output` answered with whichever was registered first. Both are now
    keyed by the declaring trait (`AssocBound.owner`), which is a latent bug
    this milestone forced into the open rather than one it introduced.
  - **`trait_self_param` has to project the bounds it is handed.** The clause is
    resolved with `Self` in scope as the trait *type*, so a binding spells its
    subject `Iterator.Item` while the default body — checked against the
    `TY_GENERIC` `Self` — spells it `Self.Item`. Two spellings is exactly what an
    equality binding cannot survive, since `ty_assoc` hands back what the binding
    named. Sabotaging this one fails 156 tests.
  - **Sabotaged eight ways in the compiler.** Deleting the nested collapse, not
    recognising its self-reference, `trait_self_param` not projecting, or
    `assoc_bounds_map` skipping the nested list each fail ~156 (std's `sum` and
    `product` drive them); the trait `Self` clause losing its binding sink fails
    257. The remaining three fail **exactly one test each**: the owner filter in
    `impl_index_assoc_type`, keying the bound list by trait, and `ty_assoc`
    matching the trait in its collapse. Clean under `make sanitize` and
    `BUILD=release`; 440 tests.
  - **Left open, and honestly:** three lines are defensive rather than pinned —
    `owner` in `type_hash`, in `types_equal`, and the canonicalisation's
    tie-break on it. No program can reach them, because an entry's owner is
    *derived* from the parameter's bounds and those are already part of the key,
    so two lists cannot differ in owner alone. They are there because the field
    is part of what an entry means, not because a test fails without them. Also
    still open: an equality is compared, never unified (so nothing infers an
    `Output`), there is no associated-type *default*, and a projection off a
    concrete base written in source (`V2.Output`) names no trait — there is no
    syntax for one — so it answers by name alone.

- **77. Downcasting a trait object** (`5c26061`) — `d as? T` -> `Option<T>`.
  Design: `language.md` "Downcasting", `architecture.md` (the coercion, read
  backwards), `runtime.md` "Trait objects". The finding: **the vtable was
  already a runtime type identity** — `Mono.vtables` memoises on exactly
  (trait ref, interned self type), so one table is one type and the test is a
  pointer compare, with nothing new in the image. The cost landed on
  *producing* an `Option` instead: it becomes the first lang item that is a
  type, because every other consumer takes one apart structurally. Remainder:
  no upcast `dyn Sub` -> `dyn Super`, which is milestone 78. Full write-up in
  the commit body.

- **78. Upcasting a trait object** (`55c9d48`) — `dyn Sub` where a `dyn Super` is expected,
  implicit and with no spelling, since the subtrait declaration is the proof.
  Design: `language.md` "Upcasting a trait object", `architecture.md` (the
  third direction), `runtime.md` "the link lives on the table". The finding:
  the two ends know different halves of the key — the *site* knows both traits
  and no concrete type, the *table* knows the concrete type and one trait — so
  the link is stored on the source table and built at compile time, which is
  only possible because `Mono.vtables` filtered by trait *is* the closed set of
  types that can be behind a `dyn Sub`. Probing turned up a live bug it had to
  fix on the way: `dyn_assoc_bindings_agree` indexed a trait's own
  `assoc_types` with a closure-numbered index, so a binding inherited from a
  supertrait was never checked and `dyn DoubleEnded<Item = String>` over an
  `Item = int` impl segfaulted at run. Remainder: none open. Full write-up in
  the commit body.

- **79. A trait object's arguments unify** (`7d115b7`) — `fun pull<T>(s: dyn Src<T>)` takes
  a `dyn Src<int>` and solves `T`, and `dyn Bag<Item = T>` does the same one
  bracket list out. Design: `language.md` "Trait objects", `architecture.md`
  ("A trait object decomposes within one trait"). The finding: `TY_DYN` was an
  atom in `infer_unify` because a `dyn`'s arguments are normally decided by the
  *coercion* that wraps a value — but two trait objects meeting have no
  coercion between them, both are already built, so within one trait a `dyn` is
  an ordinary composite like `TY_TRAIT` one level down. Across two traits it
  stays atomic, because what relates those is the upcast. 27 lines of checker,
  no codegen and no runtime change. Remainder: none of this wart; probing found
  a separate one, that no `dyn` coercion is offered inside an `if`'s or a
  `match`'s arms. Full write-up in the commit body.

- **80. Conditional pattern bindings** (`1602acd`) — `if var Opt::Some(n) = o { .. }
  else { .. }` and `while var Opt::Some(v) = it.next() { .. }`, Rust's `if let`
  and `while let` under the keyword this language already has. Design:
  `language.md` "`if var` and `while var`", `architecture.md` ("Conditional
  bindings"), `runtime.md` (same heading under "Match compilation"),
  `grammar.ebnf` `condHead`. The finding: a conditional binding is not a new
  construct but a *place to send the fail list* — the two pattern passes are
  compile_match's unchanged, and all either form adds is what failure jumps to
  (the `else`, or past the loop's back-edge). No opcode, no image change; 33
  lines of parser, 37 of checker, 95 of codegen. `while var`'s one real
  difference from every other loop is that its hidden subject local lives
  *inside* the loop, since re-evaluating the subject is what advances an
  iterator — which incidentally gives it per-iteration closure capture, where
  `for`'s one shared cell does not. Remainder: `var P = e else { .. }`
  (Rust's `let else`) has no spelling, and an irrefutable pattern in either
  header is accepted silently. Full write-up in the commit body.

- **81. Variant imports** — `use std::option::Option::Some;` binds the
  variant bare, so `Some(v)` and a `Some(v)` pattern need no `Option::` in
  front. Every spelling of it: named, braced, aliased, `pub use`d, `use E::*;`
  globbed, and qualified by a *file* or by any enum already in **scope** — one
  the file declares, imported, aliased, or preluded. `Some`/`None`/`Ok`/`Err`
  are now preluded on that same machinery, with no compiler special case.
  Design: `language.md` "Modules" + the prelude, `architecture.md` ("Variant
  imports"), `grammar.ebnf` `useDecl`. The finding: a use path's shape was
  already decided by asking whether a prefix names a *file*, and a variant is
  that question one segment further along — so discovery grew fallbacks rather
  than import kinds, and resolution grew nothing at all, since a bound variant
  *is* the whole path. What the work actually turned on was **ordering**: a
  scope qualifier can only be read once the module has resolved, so that import
  links a pass late and has to *claim its name* in source order first; and a
  glob forced the three binding strengths into the open (written > glob >
  prelude), which is also how the prelude's four names stay overridable. No
  opcode, no image change, no codegen. It also closed a pre-existing gap in
  `link_name_taken`: a module qualifier lives in no scope, so a later import of
  its name was accepted and then resolved backwards. Remainder: the import
  binds the variant and not its enum (Rust's rule); there is no module glob
  (`use a::*;`) and no glob of anything but an enum; a `use` path's module
  prefix is always a file path, never a bound module qualifier; and a variant
  is still not a first-class function value (`xs.map(Some)`), which is the
  qualified spelling's limit too and not a `use` question.

- **82. A branch is a coercion site** (`d822319`) — `var x: dyn Shape = if c
  { Sq } else { Tri };`, the `match` spelling, and
  `fun pick() -> dyn Shape { Sq }`.
  Design: `language.md` "Trait objects" (the coercion positions),
  `architecture.md` "Where a coercion is offered". The finding: a `dyn` position
  is a *written list of call sites*, not something unification derives, so the
  gap was four missing calls rather than a missing rule — and a branch was
  missed structurally, since `EXPR_IF` unified its arms against each other and
  handed them no expectation at all. `dyn_expectation` sends a `TY_DYN` hint
  (and only that) into each arm, which then coerces separately and makes the
  construct's type the hint — what `EXPR_ARRAY` already did per element.
  Checker-only: a block already forwards a hint to its tail and `compile_expr`
  already wraps after any node, so no codegen, opcode or image change. Probing
  found three more sites of the same shape (a body's tail, a closure body, a
  plain `=`) and one live segfault-from-safe-source that is **not** this
  milestone's and stays open below (`+=` never checks its operator).
  Remainder: two wrong arms under one `dyn` expectation report twice, one per
  arm — the array literal's behaviour, and each arm is its own claim.

- **83. A compound assignment is its operator** (`692c789`) — `v += w` reaches
  `impl Add for V2`, `f += 1` is legal because `f = f + 1` is, and `c += 'y'`
  no longer prints a reinterpreted bit pattern. `%=` joins the set. Design:
  `language.md` `std::ops` (the compound paragraph), `architecture.md`
  ("A compound assignment asks the same question"), `runtime.md`
  ("Compound assignment"), `grammar.ebnf` `assign`. The finding: `EXPR_ASSIGN`
  checked `a op= b` by **unifying its two operands with each other**, which is
  neither the operator's question nor the assignment's, and implies both only
  when the type is numeric — so the arithmetic opcodes, whose `else` branch
  reads both operands as doubles with no tag test, were reachable without
  anyone asking. `resolve_arith_op` is now that question factored out of
  `EXPR_BINARY`, and the place check is the second half of `a = a op b`. The
  cost is exact and worth keeping: the unsoundness and the inference were the
  same line, so `|n| { total += n; }` now needs its annotation for the reason
  `total = total + n` always did. Codegen already evaluated the place once and
  still does; a trait operator parks its call on `ExprAssign.op_call` and
  pushes the callee under the place's value, which is the only new stack shape.
  Remainder: the compound *bitwise* operators, closed by milestone 104.

- **84. A binding whose failure leaves** (`7e92800`) — `var Shape::Rect(w, h) = s else {
  panic("not a rect"); };`, Rust's `let else`: the pattern is refutable and the
  success case stays at statement level, so `w` and `h` are ordinary locals
  below it rather than names nested inside an `if var`. Design:
  `language.md` "`var ... else`" + the `!` paragraph in `std::panic`,
  `architecture.md` ("Binding patterns"), `runtime.md` ("Destructuring a
  `var`"), `grammar.ebnf` `varDecl`. The finding: the construct needed nothing
  new anywhere — `compile_destructure` already emitted the failure path, and
  all the `else` does is sit between its `OP_POP` and its `OP_MATCH_FAIL` —
  but it needed the language to *have* a notion of divergence, and what stood
  there was one line reading "the last statement is a `return`". `block_diverges`
  is that rule widened to `break`/`continue` and to any statement typed `!`,
  which is what makes `else { panic("no"); }` count with its semicolon. The
  refutability check is `matrix_covers` with the answer turned around, so the
  two spellings of `var` partition the patterns between them. No opcode, no
  image change. Remainder: a `-> !` body that falls through is still
  accepted (milestone 85), which is why the trap stays under the `else`.

- **85. A promise nothing was asked to keep** (`f9ca510`) — `fun evil() -> ! { }`
  compiled, handed its `()` to whatever the caller declared, and crashed the
  VM on an assertion. Design: `language.md` "`std::panic`" + two "not yet
  implemented" rows, `architecture.md` (inference intro), `runtime.md`
  ("Destructuring a `var`"). The finding: `!` was not under-checked, it was
  **checked in a direction that does not exist**. `infer_unify` returned true if
  *either* side was `!`, and only one of those is a rule — `!` is the bottom
  type, so it flows out of diverging code into anything and into nothing at all.
  Making it directional cost three lines and broke everything, because the
  positions that unify **siblings** (an `if`'s arms, a `match`'s, an array's
  elements) have no expectation to put first; they must join on `!` themselves,
  which `EXPR_IF` was already doing one line *after* it asked. The same
  direction, read the other way, fixed a live rejection: a monomorphic call
  compares with `types_equal`, which knows nothing of `!`, so `take(panic("no"))`
  had never been allowed. The sharp bit is placement — the rule sits *above* the
  unknowns, since divergence satisfies an expectation without being evidence
  for one, and solving `T` from it makes `first(panic("x"), 4)` reject its own
  second argument. No codegen change and no image change: a `!` is a type nobody
  produces, so there was never anything to compile. Remainder: `while true { }`
  is not divergence, so an endless function still ends in a `panic` —
  milestone 86.

- **86. The loop that asks nothing** (`02ee9be`) — `loop { }`, and a body with no
  `break` in it types `!`, so `fun serve() -> ! { loop { } }` compiles.
  Design: `language.md` "Divergence and `!`" + the expression list + two
  "not yet implemented" rows, `architecture.md` (`CheckLoop`), `runtime.md`
  (`compile_loop`), `grammar.ebnf`. The finding: the keyword **is** the
  analysis — reading `while true` as endless means reading a condition's
  *value*, and dropping the header deletes the question instead of answering
  it. What was left needed a loop **stack** in `CheckCtx` where a depth counter
  stood: a depth says "in a loop", which is all `break` ever asked, while "does
  a break leave *this* loop" needs the frame. Codegen is `compile_while` minus
  the condition. Spent immediately: 15 `while true` loops in `std/iter.dt`
  dropped their dead trailing `return`. Remainder: `break` carries no value, so
  a searching loop still assigns to a `var` above it — milestone 87.

- **87. The break that leaves with something** (`af0b594`) — `break x;`, so a
  `loop`'s value is the join of its breaks. Design: `language.md` (the
  expression list + the syntax summary), `architecture.md` (`CheckLoop`,
  `parse_block`), `runtime.md` (`compile_loop`, the break), `grammar.ebnf`. The
  finding: this is the *same question* milestone 86 answered, read at the other
  end of its range. A `loop` types `!` because its breaks are its only exits and
  there are none; it types `int` because its breaks are its only exits and they
  all say `int` — so the join **replaces** `ExprLoop.has_break` rather than
  joining it, and `NULL` after the body is the divergence case with nothing
  special written for it. Every break is a *sibling*, milestone 85's rule
  applied to however many a loop has instead of to two arms, which is why a
  `break panic("x")` is evidence for nothing and a loop whose breaks all diverge
  is still `!`. The refusal in a `while`/`for` is about the **other exit**, not
  the break: finishing carries no value, so there is nothing for one to join
  with. Codegen needed no opcode and no image change — `OP_SLIDE` already
  removes n values beneath the top, which is exactly a value evaluated before
  the body locals it reads and outliving them. Probing turned up the real cost:
  a `loop` was not on `parse_block`'s tail-promotion list, so `fun f() -> int {
  loop { break 1; } }` was `()` until it was added. **No spend in std**: every
  search there is a whole function body, so `return` already carries the value
  out, and this pays where a loop is one step of a larger function instead.
  Probing also found a **pre-existing wrong-answer bug unrelated to any of
  this** — a block with a local mis-slots it under a pending temporary, which
  is milestone 88.

- **88. A slot is a position** (`cac1586`) — a local declared while anything else is
  pending on the stack read the wrong slot: garbage, a hang, or a VM assertion,
  from safe source, in *every* such position. Design: `runtime.md` "Codegen
  shapes". The finding is one sentence and the whole milestone: `cg_add_local`
  handed out `Cg.local_count` as the frame slot, which is the value's position
  only while the stack holds nothing but locals. That is true in the language
  this compiler was cribbed from, where blocks are statements; here a block is
  an **expression**, so a `var` can be declared under an operand, a callee, an
  element, or a compound assignment's duplicated target. The identity had been
  load-bearing in four places — the slot, the upvalue's index, `cg_close_scope`'s
  operand, and the `break`/`continue` pop counts — and each was wrong in the
  same way. Separating the two (`CgLocal.slot` versus the index; `Cg.depth`
  versus `local_count`) is the fix, and what makes it cheap is that
  `compile_expr` can set the depth on the way out without asking the case what
  it emitted: an expression leaves exactly one value. So operand sequences
  account for themselves, drift cannot escape one construct, and only the raw
  traffic *between* expressions is hand-counted. Two rules had to be written
  down that were never stated before: a branch target resumes its **fork's**
  depth rather than the fallthrough's, and an unwind target is a *depth* rather
  than a count of locals. Also closes a truncation hazard the split creates —
  the one-byte operand bounds the slot, which now outruns the count. 22-case
  matrix plus a second pass over every scope-opening construct; both are pinned
  as `tests/run/local_slots_under_temporaries.dt` and
  `tests/run/break_under_temporaries.dt`, and both crash or hang at `7c1be4f`.

- **89. Advice needs an audience** (`f216cda`) — the first diagnostic below error:
  an irrefutable `if var`/`while var` header (milestone 80's remainder) and code
  below a diverging statement both warn. Design: `overview.md` repo layout,
  `language.md` "`if var` and `while var`", `architecture.md` "Diagnostics".
  The severity was already there — `DIAG_WARNING`, `diag_warning`, and the
  "warning" branch in `diag_report` all predate this and had never been called,
  and the pipeline already gated reporting on `diag_has_diags` and failure on
  `diag_has_errors`, so the first warning came out working. What was missing is
  that a warning has an **audience**: an error is a fact about the program, but
  advice presumes someone who can act on it, and the embedded std is compiled
  from source into every program with nobody to advise. So warnings are dropped
  at emission for std rather than filtered at report time, which keeps
  `diag_has_diags` honest. The predicate that decides it is `mod_is_std_module`,
  written for `@lang` and needing no change: a module reached through
  `use std::x` is keyed `<std>/x.dt` while the same file named on the command
  line keeps its real path — so std warns while you edit it and is silent while
  you merely use it, with no new concept. **Milestone 90 corrects that last
  step**: keeping a real path is what made the entry a *second module*, so the
  escape hatch worked by the same accident that broke five files outright. The
  audience is now "not std, or the root". Two things fell out of probing: a
  suppressed warning must suppress the notes that follow it, since a note
  attaches to the diagnostic before it *by position* and would otherwise orphan
  onto whatever was reported last (`DiagBag.last_dropped`); and the test harness
  had no bucket for a warning at all — exit 0 with non-empty stderr is the one
  cell of the matrix `tests/pass` and `tests/fail` leave out, hence `tests/warn`.
  The unreachable warning fired **zero** times across the suite and std, because
  milestone 86 had already deleted the dead trailing `return`s that were its
  only population; it is pinned deliberately instead.

- **90. One file, two keys, two modules** (`9e821f5`) — `./build/ducktape std/<f>.dt`
  lints a standard library file for real, and `make test` lints all 18. Design:
  `architecture.md` "Naming a std file as the entry point" and "Diagnostics";
  `CLAUDE.md` "Standard library". The finding is that **a module's identity is
  its registry key**: the same source keyed `std/cmp.dt` and `<std>/cmp.dt` is
  two modules, each with its own copy of every type and trait the file declares.
  Five files said so ("conflicting definitions of method" — the ones whose
  inherent impls land on *interned* types, `[T]`/`char`/`String`/`StringBuf`),
  and that visible failure was the lesser half: the six others in the prelude's
  closure type-checked a **shadow** std against the real one and reported
  success, because two copies of `trait Ord` are two `TraitDef`s and coherence
  had nothing to object to. `file_path` had been answering the registry key, the
  diagnostic label, where the source comes from *and* "is this std?" all at once
  — split into `alias_path` (a second key the root also answers to) and
  `std_name` (the `@lang` right, the prelude exemption, the warning audience).
  Adoption is lexical and needs both halves — a parent component named `std` and
  a stem the binary embeds — and the source still comes from **disk**, so a lint
  reads the file being edited rather than the copy `make` last mirrored. It
  cannot introduce a cycle: merging a duplicate node back into std's own import
  graph leaves an already-acyclic graph. Measured: 18/18 lint clean with zero
  warnings, +20 suite checks. Left over: the user-owned `std/` directory case
  (see warts) reports the consequence and not the cause.

- **91. Two resolvers, one file** (`2c0071c`) — std nests:
  `use std::collections::hashmap;`. Design: `language.md` "The standard
  library" and "`std::collections::hashmap`"; `architecture.md` "The embedded
  standard library" / "Which segments name the module"; `overview.md` layout.
  The four flat sites collapse to one question asked twice, and **the milestone
  is making the two askers agree**. A module's table name became its path
  relative to `std/`, which is what kept the cost low: the name was already the
  shape a key wants, so `std_mod_key` and `<std>/…` needed no change at all, and
  only the unknown-module note translates `/` back to `::`. `std_module_prefix`
  then asks the embedded table for the **longest prefix** that names a module —
  deliberately `mod_prefix_exists`' question, since two resolvers that must
  agree about one file can only be made to by asking the same thing — and the
  segment arithmetic after it is measured from there rather than from a fixed
  two. Adoption widened from "the parent component is `std`" to "an ancestor
  is", nearest first.

  Migrated only what the evidence determined: `collections` (533 lines, two
  public types, the set written on the map) into sibling modules, and `strbuf`
  into `std::string::buf` — a compound name that was only ever compound because
  std was flat. **The second one is what makes the milestone testable.** Nothing
  distinguishes longest-prefix from first-match unless some module both exists
  *and* has a child, and `std::string` + `std::string::buf` is that case:
  sabotaged to first-match, 388 tests fail with `<std>/string.dt imports
  <std>/string.dt`. Sabotaging adoption instead is caught by the new
  `check_adopted` and, independently, by milestone 90's signature — a conflicting
  inherent impl "in module `<std>/string/buf.dt`", a second module minted for
  one file.

  **`core` was planned here and is not done, on the evidence.** The prelude
  closure is computed, but what it computes is a *dependency* fact, not a
  taxonomy: it puts `array` and `string` beside `ops` because both happen to be
  reachable, and it lengthens the most-typed names in the library to shorten
  nothing. Rust's analogue is a separate crate spelled `core::cmp`, never
  `std::core::cmp`. Also found: `pub use` already existed, so a directory *can*
  be a module — `std/collections.dt` is a facade beside its own directory and
  `use std::collections::HashMap;` still works. Left over: nothing generates or
  checks that facade (see warts).

- **92. What nothing names** (`9c14c14`) — `unused variable` and `unused import`,
  the first warnings that have to *analyse* rather than notice. Design:
  `architecture.md` "What is never named"; `language.md` "Statements and
  blocks" (the `_` opt-out) and "Modules"; the gaps table.
  The finding is that **a `use` binds a name and widens the impl set, and only
  the first is visible where it is spent** — `self.compare(x)` names no import.
  A name-only rule got 5 of std's 12 unnamed module imports wrong (removing one
  costs up to 531 errors), so `ImplIndex` gained a `used` array set at
  selection, and the question became **would removing this line lose an impl
  that was selected** rather than "can it see one": reachability is transitive
  and the prelude carries most of std, so the second credits everything. 14/14
  verdicts then matched a delete-and-recompile probe. Sabotage bites both ways
  (`tests/run/import_for_impls` for the impl vote, `tests/warn/unused_import_
  redundant` for the *sole*-source part, which a first no-op attempt missed).
  Cost: 9 dead imports out of std, ~150 test bindings prefixed `_`, and
  `std/iter.dt`'s deliberate restatement of an import it only wanted for impls,
  which is now unwritable. Also found: `UseAlias` came out of the parser's bare
  form unzeroed — invisible until a `bool` was read from it (sanitizer).
  Left over: no `#[allow]`; unused *items* unreported; warnings come out in
  scope-close order, not source order.

- **93. A warning you can answer** (`3df19d3`) — `@allow("unused_variable")` on a
  declaration silences that lint over it and everything inside. Design:
  `language.md` "Warnings and `@allow`" + the gaps table, `architecture.md`
  "Diagnostics", `grammar.ebnf`. The finding is that **suppression is mostly
  naming**: a format string cannot be asked about, so the content of the
  milestone is `DiagLint` and the one `lint_names[]` table standing behind three
  things at once — the `warning[unused_variable]:` the report now prints, the key
  `@allow` matches, and the "available:" list a typo gets. Once the four warnings
  have names the policy is nearly free, because milestone 89 already built the
  door: dropping at *emission* rather than filtering at report keeps
  `diag_has_diags` honest, so `@allow` is one more term in the same `if` and an
  allowed warning takes its notes with it via the existing `last_dropped`. The
  mask **unions** rather than replaces, which is the whole of "an `@allow` on an
  `impl` covers its methods", and it resolves in the *parser*: unlike a
  `@native` key, a lint name indexes a table the compiler already owns, so
  nothing later has to look it up. That is also why it is the one parser error
  that does not enter panic mode — the attribute is well-formed and merely names
  nothing, so the declaration behind it still reads. Spent immediately:
  `std/iter.dt`'s two deliberate `use` lines, unwritable since 92, are back with
  an `@allow` that says why. Left over: no `-W`/`-Werror`, and an allow's grain
  is a whole declaration (see warts).

- **94. The declared module tree** (`bc5d325`) — `mod x;` / `pub mod x;` replaces
  path resolution. Design: `language.md` "Modules" + the gaps table,
  `architecture.md` "Modules" / "The embedded standard library",
  `grammar.ebnf`, `overview.md`. The three failures it set out to fix — a
  path's meaning depending on which files exist, a scope import reachable by
  accident, no build unit — are fixed and pinned (`tests/pass/mod_declared`,
  `tests/fail/mod_orphan_checked`).

  The finding is that **declaring the tree deletes the questions rather than
  answering them**: nothing in resolution asks whether a file exists, so
  `mod_prefix_exists`, `std_module_prefix`, `std_name_for_entry`, `mod_adopt_std`,
  `Module.alias_path` and `modreg_find` are all gone rather than rewritten, and
  the lint hatch became a flag (`--std-module std::cmp`) with no adoption to
  explain. What replaced them is `mod_walk`, used twice — once to *reach*
  modules during discovery and once to *resolve* against the finished tree —
  because two walks that must agree can only be made to by asking the same
  question (milestone 91's lesson, applied by construction this time).

  Two things the design did not say. **Discovery has to be a fixpoint**: both
  edges out of a module live in its AST, so a path two library modules deep
  needs the first parsed before the second can be named. And **the build unit
  is per tree** — the program's is registered whole, so a module nothing
  imports is still checked, while a library module joins the build when a path
  reaches it. Eager std would compile 20 modules where the prelude closure
  needs 11; measured, a trivial compile is unchanged.

  `mod` and `use` being different edges showed up as a name collision:
  `use std::string::buf;` beside `mod buf;` is not a redeclaration but both
  edges drawn at once, so `link_name_taken` exempts exactly a module naming its
  own child under its own name. The sanitizer caught `memcpy(dst, NULL, 0)` —
  the program root's label is an empty view with a NULL pointer. Migration:
  38 test roots, 2 new intermediate modules, `std/lib.dt`, and no file moved.
  Left over: `pub mod` parses and is recorded but is not enforced (milestone 95,
  see warts).

- **95. The module boundary** (`65a8651`) — `pub mod` starts meaning something, and
  a `.dt` file nothing declares says so. Design: `language.md` "Modules" +
  "Warnings and `@allow`", `architecture.md` "Modules". Pinned by
  `tests/fail/mod_private_path`, `tests/run/mod_private_subtree`,
  `tests/fail/std_private_module`, `tests/warn/orphan_module`.

  The finding is that **privacy is checked after the walk rather than during
  it**. Discovery has to *reach* a module to say anything about it, and the two
  other readings of a walk that stopped — the path names no module, the module's
  source is missing — have their own answers and must be settled first. So
  `mod_first_private` runs on the finished path, and reports the component
  nearest the root rather than the one that stopped the reader.

  Spent on std, which is what the milestone was for: `std::collections`'s
  `hashmap` and `hashset` are private now, so `HashMap` has exactly one path and
  which file holds it is the group's business. That is the *only* place in std
  where privacy was a real decision — every other file is API — and finding that
  out is the answer to "decide std's surface" rather than a substitute for one.
  Left over: those `pub use` lines are now the sole path to the types below the
  façade and nothing checks them (see warts); `orphan_module`'s `@allow` is
  written one file up, on the `mod` that gave the directory an owner, because
  the file it is about is not part of the program and has nothing to mark.

- **96. The loop you can name** (`f5c97e7`) — `'outer: loop`, `break 'outer v;`,
  `continue 'rows;`. Design: `language.md` (the expression list + the syntax
  summary), `architecture.md` (the scanner's fork, `check_loop_target`),
  `runtime.md` (`cg_loop_target`), `grammar.ebnf`. Pinned by
  `tests/run/labelled_break.dt` and six `tests/fail/label_*`.

  The finding is that **milestone 88 had already paid for this**: it made an
  unwind target a *depth* rather than a count of locals, and a depth belongs to
  a frame — so `cg->depth - loop->break_depth` counts everything stacked since
  *that* loop began whichever frame it is handed. Codegen's whole cost was
  choosing the frame; the arithmetic, the close-then-slide, the opcodes and the
  image are untouched. The scanner paid instead: a quote opens two tokens and
  only one closes, so `'a'` and `'a` are told apart by the character *after* an
  identifier, with `'ab'` left on the character branch so it still reports the
  one-character rule. A shadowed label is an **error**, not innermost-first
  resolution — the outer loop would be unreachable by name and renaming is free.
  Sabotage 5/5 bit, but **the first attempt at the fourth was a no-op** (597/597):
  a labelled break's hint only matters where the value cannot type without it,
  since `check_flow_into` re-derives the coercion from the target anyway — so
  the test had to be sharpened to an `if` whose arms are two different impls.
  Left over: no label on a plain block (milestone 98), and no `unused_label`
  lint (milestone 105).

- **97. A warning you can insist on** (`9152764`) — `-Werror`, `-Werror=<lint>`,
  `-Wno-<lint>`, `-W<lint>`. Design: `language.md` "Lint levels from the command
  line", `architecture.md` Diagnostics, `overview.md` (the CLI + the `#! flags:`
  directive). Pinned by four `tests/{fail,warn}/werror*`, two `tests/pass`, and
  `tests/fail/wflag_unknown.dt`.

  A lint gets a **level** — allow/warn/error — and the level is chosen at
  milestone 89's emission door, where `@allow` already went. That makes the
  feature nearly free: `diag_add` already counted a `DIAG_ERROR` into
  `error_count`, and `error_count` already decided the build, so escalation
  needed no new failure path. **What it exposed instead is that a phase must ask
  the bag, not only its callees** — `orphan_module` is emitted during discovery,
  which read `mod_declare_children`'s return value, so an escalated orphan
  printed `error:` and exited 0 until `compiler_load_module` folded in
  `diag_has_errors`. The other finding is the ordering: the audience outranks
  `-Werror` *more* strongly than it outranks advice, since a build failing over
  a line in the embedded std leaves its author nothing to fix — so `-Werror`
  means "**your** warnings are errors" — and `@allow` outranks it too, because a
  blanket does not know what the author of one declaration knew. Modelling a
  level as one value rather than enabled-plus-fatal is what holds the flag set
  to four: `-W<lint>` is also the "stop being fatal" spelling, so there is no
  `-Wno-error=`. Sabotage 7/7 bit. Left over: no `forbid`, and `-W` takes no
  path, so a level is all-or-nothing over a compile.

- **98. The block you can leave** (`d7b207f`) — `'a: { .. break 'a v; .. }`.
  Design: `language.md` "Statements and control flow" (the labelled-block
  bullet), `architecture.md` parser notables + the `CheckLoop` section,
  `runtime.md` `CgLoop`, `grammar.ebnf` `labelled`/`block`. Pinned by
  `tests/run/labelled_block.dt`, `tests/pass/block_label_types.dt`, four
  `tests/fail/block_label_*`, and a case in `tests/warn/unreachable_code.dt`.

  A label may now name a block, which gives one an early exit carrying a value.
  **THE FINDING: milestone 87's rule was never about having one exit — it was
  about the other exit having a value to agree with.** A `while` cannot take
  `break x` because finishing brings nothing to the join; a `loop` can because it
  has no other exit; a block can because its other exit is its *tail*, which
  carries one. So the tail is not a special case to be reconciled with the
  breaks, it **is** a break — `check_join_exit` was factored out of `STMT_BREAK`
  and the tail handed to it, after which "no exit ⇒ `!`", the `dyn` expectation
  per exit, `!` skipped as a sibling and poison absorbing are all inherited
  rather than restated. Codegen was the same story one level down: a block's
  ordinary exit already leaves its value at the depth the block opened at, which
  is where a break's slide leaves one, so the landing pad *is* the block's exit —
  one frame, one round of patching, **no opcode, no image change, no new
  arithmetic**. What had to be *decided* rather than derived is the two things a
  label must not take away: an unlabelled `break` skips block frames (else
  labelling a block steals the enclosing loop's `break`), and `continue` refuses
  one outright (a block has no next turn). The skip is written twice, in
  `check_loop_target` and `cg_loop_target`, which is milestone 91's "two
  resolvers agree only by asking the same question" — and sabotaging either half
  bites. Sabotage 7/7 bit, but **the depth one was a no-op at 612/612 first**:
  nothing in the suite opened a labelled block with a value already pending, so
  `break_depth = local_count` was indistinguishable from `= cg->depth` until a
  test put one in an argument. Left over: no `unused_label` lint (milestone
  105).

- **99. The types you cannot name** (`f4dec6d`) — `int`/`float`/`bool`/`char`; `()`
  and `!`. Design: `language.md` "Types" (the case rule, the two punctuation
  types, the reserved-name rule), `architecture.md` `type_named_builtin`,
  `grammar.ebnf` `type`. Pinned by `tests/pass/reclaimed_type_names.dt`, four
  `tests/fail/builtin_name_*`, and the reworded `tests/{pass,fail,run}/never_*`.

  The scalars are lowercase, so capitalisation now says where a type came from:
  lowercase is the language's own, PascalCase is declared or stands for one.
  `Unit` and `Never` stopped being names at all — the unit type is spelled `()`
  and the never type `!` (a new `TOKEN_BANG`; negation is still `not`), both
  parsed as their own type nodes and returned straight from `parse_type`, so
  neither can be qualified and neither has a second spelling to drift from.
  **THE FINDING: the reserved-name rule cannot cover `mod`, and std is the
  proof.** `std/lib.dt` declares `pub mod char;` and refusing it broke every
  compile — but `std::char` is fine, because its content is all `impl char`, so
  the bare `char::to_upper` spelling reaches what it meant *through the builtin
  type*. What has no route is a module item the type does not own. So the rule
  binds only where a *type* name is bound (`struct`/`enum`/`trait`/type
  parameter, which `fun` and `var` escape because a value lookup answers first),
  and the module case became a note on the failure it causes instead — two
  mystifying diagnostics turned into one that names the cause. The type
  parameter was the worst of the set: `fun id<int>(..)` reported "cannot infer
  type for 'int'" at the *call*. Both sites call `type_named_builtin`, so the
  list cannot fork. Sabotage 5/5 bit. Left over: a builtin-named module is still
  only reachable through the type.

- **100. The module glob** (`014eaf5`) — `use a::*;` binds every item `a`
  exports, and `pub use a::*;` re-exports them, which is what makes a facade
  writable.
  Design: `language.md` "Modules", `architecture.md` "Module globs",
  `grammar.ebnf` `useDecl`. **THE FINDING: the three binding strengths m81 wrote
  down are three PASSES, not a flag.** A scope lookup returns the first match,
  so a binding's strength already is its position — written, then glob, then
  prelude — and the item scopes needed none of the `from_glob`/tombstone
  machinery the variant table grew for the same job. `mod_walk_exports` is one
  traversal with two clients (bind each name; does `a` export this one?), since
  two resolvers can only be made to agree by asking the same question. Spent on
  std: `std/collections.dt`'s facade is now `pub use …::*;`, so a type added
  below its private children is exported by construction. Sabotage 8/8 bit —
  **two were no-ops first**, and each named a real test gap: nothing globbed a
  module that itself globbed, and nothing globbed one with a `pub mod` child.
  Remainder: a module qualifier is still not re-exportable, so a facade can
  offer every type below it and not the module they came from.

- **101. A `String` is valid UTF-8** (`98257b6`) — the guarantee, paid for at the
  two untrusted intakes and at `slice`, plus `matches_at`, the byte compare a
  search needs once cutting is no longer free.
  Design: `language.md` "Characters and text" + the gaps table, `runtime.md`
  "A `String` is valid UTF-8", `architecture.md` "std is embedded".
  **THE FINDING: the guarantee's cost is not the check, it is that searching
  had to stop cutting.** `std::text` compared a candidate window by slicing one
  out at *every* byte offset — offsets it had no reason to believe in yet — so a
  boundary-checked `slice` would have made `find`, `split`, `starts_with` and
  `ends_with` fail on inputs that used to answer `false`. `matches_at` is total
  where that cut is not, and UTF-8's self-synchronisation is what makes it so: a
  non-empty needle begins with a lead byte, a lead byte never sits where a
  continuation byte belongs, so a compare inside a character simply says no.
  The same property pays a second time — an offset `find` returns is therefore
  always a boundary, so `split` cuts at its own matches with nothing to check —
  and a third: deleting the interned window made the search ~21% faster on
  ASCII and more where windows do not repeat. Enforcement is three places
  (`read_file`, the image string table, `slice`); everything else is valid by
  induction, `StringBuf` included. What that deletes is a whole population of
  runtime errors — the three `tests/fail_run` files that walked or padded a
  halved character are gone, and `string_char_at`/`string_prev_boundary` now
  report "not a character boundary", a question about the offset rather than
  about the string. Sabotage 6/6 bit **but one was a no-op first**, and the
  no-op is the finding: `utf8_is_boundary`'s `at == len` arm cannot be made to
  matter from ducktape, because an `ObjString` is NUL-terminated and a NUL is
  not a continuation byte — it is there so the helper does not depend on that.
  Remainder: a position is still a bare `int`, which is milestone 102.

- **102. The opaque `StrPos`** (`2e23800`) — a position comes from an end of a
  string or from a match, never from an `int`, and carries the String it names.
  `slice` and `matches_at` moved to `std::text` with it; `split_once`,
  `take_chars` and `drop_chars` are what the opacity made necessary.
  Design: `language.md` "`std::text`" + "Characters and text" + the gaps table,
  `runtime.md` "A `String` is valid UTF-8".
  **THE FINDING: the roadmap's design question dissolved instead of being
  answered.** It asked how `std::text`'s offset arithmetic (`n - sl`, `i + sl`)
  should be spelled once a position is opaque, warning that a `StrPos` with
  arithmetic is an `int` wearing a hat. But a private field is opaque *to other
  modules*, and only the module owning it can mint one — so the module that
  mints positions must also be the one that cuts and searches, and inside that
  wall the arithmetic never changed spelling at all. Which is exactly the
  discipline `CharIter` has kept over its own offsets since milestone 61, and
  the reason `std::array`'s `pop_last` is a free function: an impl method has no
  visibility control, so the raw natives had to be private free functions too.
  A second finding decided the shape: **opacity alone leaves a silent wrong
  answer** — `a.slice(b.start(), b.end())` is in bounds, on boundaries, and
  answers nonsense — so a `StrPos` carries its String, checked by pointer
  equality because interning makes equal Strings one object (which also makes
  accepting an equal string *correct*, not merely permissive). Measured, best of
  5-7 at `-O2`: the position path costs +57% over milestone 101's `Option<int>`
  and the identity field is +10.7% of that, while `split` — which mints no
  position — came out **8% faster**, having lost a method dispatch. Two of
  `slice`'s three runtime errors are now unreachable from safe source and are
  tested through a program that binds the raw `@native` itself. Sabotage 9/9
  bit, and the ninth is a property rather than a test: leaving the old
  int-taking `slice` in `std::string` is refused by milestone 68's coherence
  rule ("conflicting definitions of method 'slice' for type 'String'"), so the
  language made this a *move* rather than an addition.
  Remainder: `slice` takes two independent positions rather than a range, so a
  reversed pair is still a runtime error; and there is no reverse search.

- **103. `String` → `string`, `Range` → `range`** (`b0f622d`) — the naming half the
  positional work left. The case rule now has no exceptions: lowercase is an
  immutable value with no identity, PascalCase has identity or was declared, so
  `StringBuf` keeps its capital because which buffer you hold is observable.
  Design: `language.md` "Types" + "Module-qualified paths" + the gaps table.
  479 lines across 143 `.dt` files, plus `type_named_builtin` and the two type
  printers; everything else was comment and prose.
  **THE FINDING: a rename with no semantic content found a false diagnostic.**
  Milestone 99's escape hatch for a builtin-named module — "it is reached
  through the type anyway" — held only because `std::char` is all `impl char`.
  Renaming the type made `std::string` the second such module and the first one
  that bites: its free builders (`join`, `concat`, `from_chars`) stay free
  because their receiver is a `[string]`, so nothing owns them, and
  `string::concat(..)` now resolves the *type*. The note pointed the way out
  "through their full path" — which is not a spelling the language has, since a
  qualifier must be a bound single segment. The remedies that exist are
  `use std::string::concat;` and `use std::string as strings;`, and the note
  says that instead; `tests/fail/builtin_name_module_std.dt` pins it.
  Remainder: the shadowing itself still stands, now with a std module behind it.

- **104. The compound bitwise assignments** (`4eb7dbc`) — `&= |= ^= <<= >>=
  >>>=`, milestone 83's remainder and the last of its family. `&=` `|=` `^=`
  fuse in the scanner;
  the three shifts are *runs* the way `>>` is (`>>=` scans as `>` `>=`), so
  they belong to `parse_assign` while opening with a token `parse_shift` and
  `parse_comparison` both want — hence `at_shift_assign`, a predicate those two
  levels consult in order to decline. Design: `language.md` "Operators" +
  "std::ops", `architecture.md` "Parser" + "the bitwise half of the same
  sentence", `runtime.md` "Compound assignment", `grammar.ebnf` `assignOp`.
  **THE FINDING: the milestone's own thesis had a counter-example inside the
  compiler.** Milestone 83 factored `resolve_arith_op` so that `a + b` and
  `a += b` could not disagree — but `cg_compound_end` still owned a *second*
  map from `+=` to `OP_ADD`, parallel to `compile_binary`'s from `+` to
  `OP_ADD`, and mapping `>>=` to `USHR` in the checker left the suite green
  because the checker's copy only decides which *family* an operator is in.
  Both now read one `binary_opcode` after one `token_compound_binary_op`.
  **SECOND FINDING: milestone 83's stated cost comes back for this family
  alone.** A bitwise operator's operand type is known before its operands are,
  so it unifies where the arithmetic one can only check — and unifying solves:
  `|n| { mask |= n; }` type-checks unannotated while `|n| { total += n; }`
  still cannot. Sabotage 8/8, but **two were no-ops first**: the opcode one
  above, and an adjacency test whose fail case I had written as `a >> = 1`,
  which is refused by a different mechanism (`a > >= 1` is the real one).
  No new opcode, no image change. Remainder: none.

- **105. `unused_label`** (`016a10f`) — milestone 96's remainder and the smallest of
  the lint family: one enum entry, one name string, one `bool` on `CheckLoop`.
  Design: `language.md` "Expressions" (labels, and the block paragraph) + the
  lint table, `architecture.md` `check_loop_pop`. The name string is the whole
  registration — `@allow("unused_label")`, `-Werror=unused_label` and
  `-Wno-unused_label` all worked before any of them was written, which is
  milestone 89's table earning its keep a third time.
  **THE FINDING: the lint's first act was to fail three tests, and all three
  were right.** `tests/pass/block_label_types` and `tests/run/labelled_block`
  hold labels written *to be inert* — one pinning that a labelled block nothing
  breaks to still types `!`, one pinning milestone 98's rule that a bare
  `break` names the loop and skips the block. A label that must go unread is
  exactly what that rule can only be demonstrated by, so the lint is correct
  and so is the code; `@allow` on the declaration is the answer.
  **SECOND FINDING: that is also the argument against a `_` hatch.** Every
  underscore opt-out elsewhere exists because some form *makes* you bind a name
  (a pattern names all its fields, a signature names `self`); nothing makes you
  write a label, and the only real consumers of a deliberately unread one are
  tests of the label mechanism itself — which `@allow`'s declaration grain fits.
  Sabotage 9/9, none a no-op, and 5–8 each bit **only** their own file: a warn
  test matches one substring, so pinning the four carriers apart needed four
  files rather than one with four cases. Remainder: a label named only from
  inside a closure reports twice (the `break` errors, and the label is then
  genuinely unread) — the m82 family, and only on a failing compile.

- **106. `array::fill` and the bulk operations** (`9ee97c9`) — the last ranked
  follow-up from milestone 63's cost model, plus the family it belongs to:
  `fill`, `with_capacity`, `reserve`, `extend`, `truncate`, and `clear`
  rewritten onto `truncate`. Design: `language.md` "Arrays" (the surface, the
  sharing rule), `runtime.md` "Growing an array" (the narrowing checks and
  `heap_alloc`) and its new cost-model section. No compiler change, no opcode,
  no image change — six declarations and five C functions.
  **THE FINDING: `fill` is the first native to let a program name an allocation
  *size*, and that is a different kind of native.** Every other allocation is
  sized by data already in hand — `push` grows by one, `concat` sums two
  existing lengths — so no native before this had to *refuse* a request. Two
  refusals: a negative length, and a length past `int` (a `count` is 32 bits and
  a program's is 64, so `(int)` of 2^33 is a silent no-op and `(int)` of 2^31 is
  a `count` that makes `len()` answer -1). Above them the audit reached
  `heap_alloc`, which returned a NULL every caller wrote into; exhaustion is now
  a defined exit rather than a segfault.
  **SECOND FINDING: `fill` cannot clone, so the naming rule is its contract.**
  There is no `Clone`, so `fill(2, [])` is two names for one array and
  `fill(rows, fill(cols, 0))` is one row shared. Milestone 103's rule is exactly
  the predicate — lowercase means no identity, so the sharing is unobservable;
  anything else has to be built in a loop. Also sharpens milestone 102's rule:
  a native cannot mint a *declared* type, but an array's shape is the runtime's,
  so `fill` builds a whole `[T]` in C.
  Measured -O2 best-of-7 over 1,000,000 elements: build 61→15ns/elem, copy
  43→16ns/elem, drop 70ns/elem→O(1). The model predicted it exactly — zero
  callbacks, so the whole ~37ns of iteration and frame goes and the allocation
  stays. **`clear`'s 70ns/elem was inside std itself**, an interpreted walk over
  `pop_last` to reach a state that is one assignment. Writing the model down
  also closed a dangling forward reference in `runtime.md` ("the cost model
  below", which was never below).
  Sabotage 8/8 attempted, **6 bit and 2 were no-ops — both honestly so**:
  `reserve` made a no-op changes nothing a test can see, because an allocation
  hint has no observable behaviour by construction, and the `heap_alloc` check
  guards a state the suite cannot construct in bounded time. That refines the
  standing rule: a sabotage that does not bite is a *test gap* only when the
  thing sabotaged has something to observe. Remainder: `truncate` and `clear`
  still never release capacity, and there is no `insert`/`remove` at an index —
  both bulk moves of the same shape, neither yet wanted.
  - **First consumer, straight after** (`072465b`): `std::collections::hashmap`'s
    `empty_slots` was the family's exact shape, and is now one `array::fill`.
    `HashMap::with_capacity(20000)` is **5.1x faster** (34 → 6.5ns per slot);
    building a map by insertion gains 11%, the rest being probe cost. It is also
    the case that shows the sharing rule is about *observability* rather than
    spelling: `Slot::Empty` is PascalCase, but a fieldless variant is an interned
    singleton (milestone 67), so the loop was already storing one object `n`
    times and `fill` is exactly equivalent. `keys`/`values` take
    `with_capacity(self.live)` in the same pass — the walk cannot be shortened,
    but its answer's length is known before it starts.

- **107. `unused_assignment`: a store is not a read.** (`ae41c2f`) A binding
  something assigns to and nothing ever reads now warns under its own name.
  `language.md` "Statements and blocks" + "Warnings and `@allow`",
  `architecture.md` "What is never named".
  **THE FINDING: the lint's first act was to find one of its own kind inside the
  compiler.** Splitting `VarEntry.used` into read and written made
  `out_crossed_fn` the only reader of `.is_captured` — and `.is_captured` was
  never read *at all*, because codegen computes its own on `Cg.locals`, which is
  the one that drives `OP_CLOSE_UPVALUE`. Deleting it left `is_fn_boundary`
  write-only; `is_loop` had been so all along; and `VarEntry.slot` /
  `next_slot` / `vscope_define`'s return value were a fourth, whose doc comment
  described a diagnostic it never emitted. **Five fields of bookkeeping nobody
  read**, invisible to `-Wall -Wextra` because C's unused-but-set warning does not
  look inside a struct. All five are gone: `vscope_push` takes a parent, a value
  scope holds no slots, and milestone 88's rule — a local's position is a *stack*
  position, so only codegen can number it — is now true of the code as well as of
  the runtime. **SECOND FINDING: the rule is decided at one site and the
  exemptions are the language's own.** `CheckCtx.write_target` marks exactly one
  `resolve_expr` call, so a compound `a op= b` and a `p.f = v` are reads *by
  construction* rather than by a list. And the one legitimate write-only binding
  in the suite was already there — `tests/run/unit_variant_shared.dt` overwrites
  a binding to drop the last reference to a GC singleton, which with no `drop` in
  the language is the only way to release one; the `_` prefix says so.
  Sabotage 8/8 attempted, 6 bit and 2 were no-ops, both honestly: a qualified
  path target fails compilation before the bare-name lookup, and clearing
  `write_target` is hygiene over a pointer nothing reads twice.
  Remainder: the grain is the binding rather than the store (see the warts), so a
  dead store into a binding that is read elsewhere is still invisible.

- **108. `Bytes`, and the first thing a program can read.** (`SHA`)
  `std::io::read_file` answers a packed `Bytes`; `string::from_utf8` is the
  checked way back to text. `language.md` "`std::bytes`, and reading a file",
  `runtime.md` "Heap objects" + "Natives", `architecture.md` "Types".
  **THE FINDING: I/O could not be added without deciding the byte question,
  because milestone 101 had already closed the easy answer.** A `string` is
  guaranteed valid UTF-8, so a read that returned one would have to *fail* on a
  file that is merely not text — which makes the return type the whole design.
  `Bytes` is a sibling of `StringBuf` with the same three fields and the same
  growth (`buf_reserve` now takes the triple, not either object): what separates
  the kinds is which door is checked, entry for one and exit for the other.
  **SECOND FINDING: a ducktape string is not a C string, and `read_file` is the
  first native to hand one to an API that assumes it is.** A string carries its
  own length, so `StringBuf` can build one holding a NUL — and `fopen` would
  have opened the file the prefix names, succeeding on the wrong file. Refused,
  and reachable from safe source, so it is a `tests/run` case rather than a
  theoretical one. Adding the type cost three inert switches and one
  `type_named_builtin` entry, and that entry alone is what refuses `struct
  Bytes` and a type parameter called `Bytes` — milestone 89's one-table lesson
  for the fourth time. No opcode, no image change: a `Bytes` has no literal
  syntax, so a chunk constant can never be one. Sabotage 18/18 attempted, 17
  bit; the one no-op restores the buffer after a *partial* read, which the suite
  cannot construct (the `ferror` branch it sits on is reachable — a directory
  opens and then refuses — and is tested). Remainder: no write side, no stdin,
  no `b[i]`, and no `reserve`/`extend`/`slice` on a `Bytes`.

## Next (in recommended order)

Estimates are relative to one focused session ≈ the checker-completion
milestone (~900 lines).

Nothing on the main line is *blocked*: every construct the checker accepts in
`tests/pass/in_fixed.dt` now also runs. The list below is what the "known
warts" section would promote first, in the order that pays off soonest — pick
by appetite rather than by necessity. Nothing here is load-bearing any more:
the module system was the one item that replaced infrastructure known to be
wrong, and it landed in milestones 94–95.

(**A heterogeneous operator** was the largest open design question and is now
milestone 75: `V2 * float`. What it needed from the language was two things
neither of which was about operators — a trait type parameter with a default,
and a bound that may name its own subject — plus milestone 30's argument-driven
selection at the *receiver* spelling. Its remainder, an `Output` associated
type, is milestone 76, which is also where the "an equality is compared, never
unified" limit stopped being a blocker and became a *cost*: `Output` works, it
just has to be written down at every bound that keeps the result, because
nothing infers one.)

1. **Growing std on top of the natives** — the mechanism landed in milestone
   16 with a deliberately small registry, and the pieces with a design question
   behind them are done: a growable `ObjArray` (milestone 23, so `std::array`
   has `push`/`pop`), a growable text buffer (milestone 24, so a `string` can be
   *built* rather than concatenated), and string ordering (milestone 25, so
   `impl Ord for string` exists and text sorts). What is left is breadth, and
   each piece is one registry entry plus a decision about the type it needs.
   Every piece with a design question behind it is now done — the last open one,
   padding, is milestone 34 (below). **Milestone 106 spent the last of the
   ranked performance follow-ups** (`array::fill` and the bulk family): what is
   left of *that* list is nothing, since the cost model declines a native map.

   (**An iterator source** was the one piece the iterator work had left
   pointing at itself, and is now milestone 60: `xs.iter()` and
   `(0..n).iter()`. The design question it recorded — what an array iterator
   holds, when the language has no borrow to hold — was answered "the array,
   and nothing snapshotted", which is what `for x in xs` re-reading the length
   each turn already said. **A `string`'s characters as a walk** is the next of
   these, milestone 61, and it answered the same question the other way for the
   same kind of reason: a string is immutable, so its iterator snapshots. What
   is left of this item is ordinary breadth. **A sorted `[T]`** is milestone 62,
   and it needed nothing from the language at all — one `.dt` file whose whole
   content is decisions. **A map or set type** was the last piece with a design
   question behind it, and it is now milestone 63: the fork was hash versus
   ordered, and hash won on the strength of what it would settle — it was the
   named consumer for the deferred `Eq`, and it answered that question in the
   negative from the inside. What is left of this item is breadth with nothing
   open in it: an ordered map over `lower_bound` is still unwritten and still
   costs no design, and `std::hash` could grow impls for more arities the way
   `std::cmp` and `std::fmt` want to.)

(**The first iterator combinators** — `map`/`filter`/`collect` — are now
milestone 47, along with driving a bounded generic or a `dyn Iterator` through
`for`; milestone 48 moved them onto `Iterator` as methods (`it.map(f).collect()`)
by partitioning object safety per method. They are ordinary `.dt` code (an
adapter is a struct with an `impl Iterator`); the one compiler change in 47 was
inference, so a closure typed by the source's `Item` projection can be checked.
`fold`/`enumerate`/`zip`/`take` are milestone 49, the same adapter shape;
`flat_map` waited on the projection-through-a-bound codegen wart, which
milestone 50 closes and milestone 51 spends, adding it and `skip`. What is left
is `chain`, and it waits on a *bound* rather than on machinery — see item 3.)

(**Padding a rendered value to a width** was the last open piece here and is now
milestone 34: `std::string` ships `pad_start`/`pad_end`/`pad_center`. The
format-spec question it was gating is answered *the functions are the primitive*
— a rendering choice is those functions, and the spec is sugar over them.)

(**The `{v:>8}` format spec** — the sugar milestone 34 recorded as the one thing
genuinely absent — is now milestone 35: a `:` spec desugars in the checker to
the `pad_*` / `float` calls, so codegen and the runtime are untouched. Its one
cost is that those four functions became lang items like `Display`, forced by
the same argument: the user never types the names, so their meaning cannot be
left to imports.)

(**Text operations** was the second open piece here and is now milestone 32:
`std::text` ships `find`, `split`, `trim`, `starts_with`/`ends_with`, `contains`
and `parse_int`. The byte-vs-character fork it left open was answered both ways
— a position is a byte offset, a classification crosses to `chars` — and the
functions had to become their own module rather than more of `std::string`,
which the dependency graph forced.)

(**Selection by argument type** was item 2 here and is now milestone 30: a
qualified `Meters::from(x)` reads its argument to pick the impl. The *receiver*
spelling that milestone 30's note left unread is now milestone 31: a
trait-qualified `into::<Fahrenheit>::into(c)` names the trait so the receiver
settles the rest, without an expected type.)

(**Ordering operators over `Ord`** was item 2 here and is now milestone 38:
`a < b` on a non-numeric type desugars to `a.cmp(b) < 0`. The open question it
recorded — how many `Ord` methods the operator names — was answered `cmp` only,
the smaller entanglement, matching how `Display` names exactly `to_string`.)

(**The associated-type-bound work is finished.** The trait half landed in
milestone 52 (`where I.Item: Ord`), the placement half in 53 (the same clause on
a trait method's signature, which made `it.max()` a combinator), the equality
binding in 54 (`J: Iterator<Item = I.Item>`, which made `chain` one), and the
*projection*-subject binding in 76 (`where Self.Item: Add<Output = Self.Item>`,
which made `sum` one again once operators grew an `Output`). What the family
still cannot do is *infer*: an equality is compared, never unified, so a binding
will not solve a type argument from the other side.)

(**`sum`/`product` are no longer blocked** — they were waiting on an `Add` trait
rather than on any predicate, and that is milestone 55: the arithmetic operators
desugar to `std::ops` the way `<` desugars to `Ord`, so `where Self.Item: Add`
is writable and the reduce is an ordinary combinator.)

(**`rev` was the last combinator with a design question**, and it is milestone
58: reversing needs an iterator that can be driven from both ends, which needs a
trait that requires `Iterator` — so the answer was **supertraits**, a language
feature rather than another adapter struct. The breadth that was left after it —
`any`, `all`, `find`, `position`, `count`, `for_each`, `take_while`,
`skip_while` — is milestone 59, and it needed no compiler change at all. **The
iterator direction is now closed**: what would extend it further is a *source*
in std rather than another combinator, since every iterator a program can drive
today is one it wrote itself.)

(**`std::assert`** is milestone 57, and is the shape "breadth" takes when a
piece needs no registry entry at all: five functions over `std::panic`, zero
compiler change, and the only decisions are where they live and which messages
cost something when the assertion holds. Its findings — that a callee cannot
name its own argument without macros, and that `assert_eq`'s `Display` bound is
bought by the *message* rather than the comparison — belong to item 3 as much
as here.)

2. **A custom equality trait (`Eq`) — the named consumer has now declined it.**
   Not on the main line, and deliberately deferred rather than planned —
   recorded here so the reasoning survives. Milestone 63 built the hash map this
   item had been waiting for and found it wanted `Hash` and nothing else: `==`
   is structural, so `k == key` type-checks on a `K` bounded only by `Hash`, and
   collision resolution is one opcode over any type whatsoever. The structural
   `==` even makes the `Hash`/`Eq` contract nearly unbreakable, since there is
   no custom equality for a hash to contradict. What would now reopen this is a
   type whose equality is genuinely not structural — a case-insensitive string,
   a value with a cached field that should not count — and nothing in std has
   one. Since milestone 55 it is the *only* operator that is
   not a trait, which sharpens the question rather than settling it: ordering and
   arithmetic went to traits because they had no meaning for a non-primitive at
   all, while equality already has one that works for every type.
   `==`/`!=` stay a structural primitive: `OP_EQ` reads
   no static type, so `a == b` works on any value — int, struct, generic `T` —
   with nothing imported. Routing it through an `Eq`/`PartialEq` trait would
   (a) make the commonest operation depend on a trait impl — and unlike `Ord`,
   `==` is deliberately *not* one of the prelude's lang items, so this would be
   real friction, not the kind the prelude removes, (b) hand coherence the power
   to take `[1,2] == [1,2]` away from a program whose modules disagree — the
   `Display`/`Ord`-for-`[T]` wart applied to `==` — and (c) cost a monomorphised
   call where an opcode
   stands now, all to buy *custom* equality that would need specialisation (which
   the language does not have) to coexist with the structural default.
   Milestone 57 supplied the nearest thing to a consumer std had then, and it
   landed on the *other* side: `assert_eq<T: Display>` compares with `OP_EQ` on
   any `T` whatsoever and spends its only bound on rendering the two values, so
   what it wanted from the language was a way to *print*, not a way to compare.
   Milestone 63's map is the second consumer to answer the same way, and it was
   the one this item named in advance. The `PartialEq`/`PartialOrd` split waits
   with it; its one live motivation, the `Ord for float` NaN wart, is an
   ordering bug fixable on its own terms and does not need the trait hierarchy
   to address — and milestone 63 showed the other way out of it, since
   `std::hash` simply declines to implement `Hash for float` at all.

3. **Growing `std::io` past one read.** Milestone 108 spent the item that stood
   here — a packed `Bytes`, shipping with the milestone that first had something
   to read — and what is left of the direction is breadth with one decision in
   it. `read_file` is a single call that appends a whole file or fails; there is
   no write side, no stdin, no seeking, and no line-at-a-time read. The decision
   waiting is whether a *handle* is worth a type: everything above can be
   written as more whole-file calls, and only streaming needs an object that
   remembers a position. `Bytes` itself is missing the bulk operations `[T]`
   grew in milestone 106 (`reserve`, `extend`, `truncate`, a slice) and has no
   `b[i]`, all of which are ordinary registry entries.

   (The **`byte` scalar** stays rejected, and milestone 108 is the evidence
   rather than the argument: a `Value` is as wide as its widest arm, so `[byte]`
   would have cost what `[int]` costs, and packing was the whole reason to want
   one. If a `byte` is ever wanted anyway it belongs to an unsigned-integer
   milestone that closes the `>>>` wart, not here.)

Not on the roadmap: the **REPL** is a side feature, not a milestone — it lives
on the `feature/repl` branch (`--repl`, incremental compilation over one module
via `Module.decl_base`) and is not part of the main line.

## Known warts to clean up opportunistically

- the **embedded std bypasses the UTF-8 intake check**. `mod_parse` takes
  `std_module_source()` when `is_std && !from_disk`, so a library module's bytes
  never pass `read_file` — the same file is validated under `--std-module`
  (which sets `from_disk`) and trusted on every normal compile. Not reachable
  today: std is ASCII and `make test` lints every module through the disk path,
  so the suite would catch it. But milestone 101's invariant rests on two
  checked intakes plus one trusted one, which is not what it claims
- **one mistake, two diagnostics**, in two places, and the same cause in both:
  a bare-path lookup introduces an inference variable nothing goes on to solve.
  An associated function written where a *pattern* expects a variant reports
  "expected an enum variant in this pattern" **and** an unsolved type parameter
  (milestone 70; a struct has always done this too). The bare
  `it.fold<int>(0, f)` parses as comparisons, so it reports an unknown *field*
  and then usually an undefined variable for the type name (milestone 71); a
  note on the first names the turbofish, which is as far as that site can see
- a label whose only `break` sits inside a closure reports twice: the `break`
  errors ("nothing labelled `'a` is in scope", since a closure resets the
  target list) and the label is then genuinely unread, so `unused_label` fires
  as well (milestone 105). Suppressing it precisely would mean keeping the
  outer target list reachable across the closure boundary just to mark it —
  machinery for a compile that fails either way
- two arms that are both wrong under one `dyn` expectation report twice, once
  each, since each arm is checked against the expectation rather than against
  its neighbour. That is the array literal's behaviour per element and is
  arguably right — two arms are two claims — but it is a second diagnostic for
  what a reader may see as one mistake (milestone 82)
- `compile_coerce_dyn`'s abstract-target guard on the upcast is unreachable —
  every site the checker can build has its target trait reference pinned, by
  the source's closure (`check_upcast_dyn`) or by `cg_subst` at the
  instantiation. It stays as a defensive net rather than a diagnostic anyone
  can reach
- sema's `MAX_BOUNDS` (16) is the last fixed cap of the family milestone 73
  cleared out of the parser. It bounds a real array and has its own diagnostic,
  so it is a limit rather than a hazard
- no unsigned integer type, so `>>>` stands in for one and a bit pattern with
  the top bit set prints negative
- **an import's impl set arrives transitively, so a deliberate `use` can read
  as unused.** `std::io` imports `std::bytes`, so `use std::io::print;` hands a
  program the whole `Bytes` surface — and `unused_import` then reports an
  explicit `use std::bytes;` beside it, correctly under its own rule (removing
  that line loses no impl that was selected) and unhelpfully as advice. The
  prelude has always done this for `StringBuf` through `std::string`; milestone
  108 is the first time a module a program imports *by choice* became a source
  of it. What would fix it is a way to say an import is wanted for its impls,
  which is the same missing spelling `language.md`'s table already records
- **the failure path of `io_read_file` is only half testable.** A read that
  fails partway restores the caller's buffer, and the suite cannot build one:
  the reachable failures (a missing file, a directory) append nothing before
  they fail, so the restore is a no-op wherever it can be observed. The
  `ferror` branch itself is tested — a directory opens and then refuses
- a *written* impl always wins, which is what keeps milestone 74's supertrait
  derivation from ever conflicting — so an item of a super that something else
  already implements may not also be written in the sub's block. The one-block
  spelling and a separate super impl cannot be mixed for the same name
- a format spec inside `{}` exists as of milestone 35 (`{v:>8}`, `{f:.3}`,
  `{f:>10.3}`), desugaring to `std::string::pad_*` / `std::fmt::float`. What has
  no spelling is a *dynamic* width or precision (`{v:>{n}}`): both are literals,
  so a runtime value in that position must still be written as the call itself.
  A bare width with no alignment (`{v:8}`) is also rejected — an alignment is
  required before a width, since defaulting it would need the value's type
- a `Display` impl whose body is `return "{self}";` recurses until the frame
  limit. That is exactly how `std::fmt`'s four impls are written, and it is
  correct there only because a primitive receiver takes the built-in path —
  the asymmetry is invisible from the source. A cycle check would have to run
  where the impl is written, not where it is called
- `std::ops` ships `impl Add for int` (and the other eleven primitive impls), so
  a program can no longer write its own — the `Display`/`Ord`-for-primitives
  cost, one module over, and unavoidable for the same reason: without them a
  bounded `T: Add` could not instantiate at a number
- `Display` ships for `[T]`, `(A, B)`, `Option<T>` and `Result<T, E>`, which
  means a program can no longer write its own for those: naming the trait makes
  the std impl visible and coherence rejects the pair. A tuple of any other
  arity has no impl, since nothing can be generic over a tuple's length
- `std::convert`'s blanket `impl<T, U: From<T>> Into<U> for T` is the same wart
  taken to its limit: it applies to *every* self type, so importing the module
  takes `into` impls away from a program entirely. That is Rust's rule and the
  price of the free direction being free, but it is the widest thing coherence's
  blindness to bounds has cost so far
- a unit struct's name cannot be bound as a variable any more: `var Marker =
  7;` is a struct pattern against an `int`, and the diagnostic ("expected
  struct type in struct pattern") describes the rewrite rather than the
  mistake. Rust behaves the same way; the message could be kinder
- no shadowing diagnostics for `var` (`vscope_define` todo); top-level item
  names do collide, but a `var` may silently shadow one in the same scope. As of
  milestone 89 there *is* a warning severity for this to use, and as of 92 the
  entry to hang it on (`VarEntry.span`, `used`) — what is missing is now only
  the analysis, and the policy call about whether shadowing deserves one at all
  (Rust does not warn on it)
- a refutable `var` binding whose column type inference never pinned down is
  accepted (the tri-state answer reports nothing) and traps at runtime via
  `OP_MATCH_FAIL` instead of at compile time
- a lint's level is **all-or-nothing over a compile**: `-W` (milestone 97) takes
  no module path, so one module cannot be held to a level the rest is not, and
  the only per-declaration control is `@allow`, which only goes downwards. Nor
  is there a level that *beats* an `@allow` — Rust's `forbid` — so a build that
  must not be silenced anywhere has to grep for the attribute
- a module may be named after a builtin type (`mod char;`, which std does), but
  a builtin name resolves before any module, so `char::` reaches the *type*'s
  items. That is why std's spelling works — `std::char` is all `impl char` — and
  why a free function in such a module has no bare path at all. Milestone 99
  chose a note over a refusal here; the shadowing itself stands, and milestone
  103 made `std::string` the second instance and the first one that bites:
  `join`/`concat`/`from_chars` now need `use std::string::join;` or
  `use std::string as strings;`. A qualifier must be a bound single segment, so
  there is no `std::string::concat(..)` spelling to fall back on either
- an allow's grain is a whole **declaration**, since an attribute is what
  carries one. There is no statement or expression form, and none on a trait
  item — a default body is covered by its trait, one scope wider than it
  should be. Rust's `#[allow]` on a statement is the shape this is missing
- an *item* nothing names — a private `fun`, `struct` or `trait` no call
  reaches — is not reported, where a binding and an import now are (milestone
  92). Codegen already skips it, so the cost is a reader's rather than a
  program's
- `PAT_WILDCARD` is **unreachable**, and milestone 92 depends on it being so.
  `scan_identifier` runs before the `switch` that would make a lone `_` a
  `TOKEN_UNDER`, so `_` is an ordinary identifier and every `_` in a pattern is
  a `PAT_BIND` named `_` — which is exactly why the `_` opt-out needed no
  spelling of its own. The cost is a pattern kind implemented in five places
  (sema ×3, codegen ×2, ast) that nothing constructs, and `var y = _;` being a
  legal read of a binding nobody meant to make
- `unused_assignment`'s grain is the **binding**, not the store, so a dead store
  into a binding that is read *somewhere* is invisible: `var x = 1; x = 2;` warns
  but `var x = 1; x = 2; print("{x}");` does not, and neither does a lone
  `counted += 1`, whose target the compound operator genuinely reads. Rust
  reports each store and needs liveness over a control-flow graph to do it,
  which is the cost this defers rather than avoids
- the unused-binding warnings of one function come out in **scope-close** order
  rather than source order, so an inner block's precede an outer one's whatever
  their lines. Sorting the bag would need notes to stop attaching to the
  diagnostic before them by position (milestone 89)
- `while true { }` still types `()`, and deliberately: its condition is an
  expression, and the checker reads types rather than values. `loop { }` is the
  spelling that asks no condition (milestone 86)
- `pub` is parsed and ignored on the `impl` keyword itself, and *rejected* on a
  method: `pub fun` inside an `impl` block is "expected impl item". **Method**
  visibility does not exist — a method is as visible as its impl — which is why
  the operations std wants to keep to itself (`std::array`'s raw `pop_last`,
  `std::iter`'s `char_at` / `prev_boundary`, `std::string`'s `char_width`) are
  private *free functions* rather than methods. **Field** visibility does exist
  and this entry used to deny it: a field takes `pub` and is private to its
  module without one
- a definition nothing reaches is never compiled, so a construct the VM
  refuses inside one goes unreported — the diagnostic arrives only if
  something calls it (`tests/run/unreachable_body.dt`). The checker is
  unaffected; this is codegen only
- overlapping method names across impls: bare generic paths take the first
  registered impl
- `Point::new` vs `Point::<int>::new`: expression paths require turbofish
- an associated-type projection keying an instantiation **through a bound** was
  the long-standing one here — handing a `T.Item` / `Self.Item` value to another
  generic (`id(v.item())`, `it.next().unwrap()` where the payload is `I.Item`)
  reported "cannot instantiate 'id': type argument 'T' is not known here" — and
  is **closed as of milestone 50**: `assoc_apply` gives codegen the collapse
  `infer_apply` was doing for the checker, and `cg_subst` applies it at every
  site that substitutes. What remained after it was narrower and *not* the same
  problem — bounding an associated type — and milestones 52, 53 and 54 answered
  it between them: `where I.Item: Ord` works on a `fun`, an `impl`, and (53) a
  trait method's signature, which is what made `it.max()` a combinator, while
  54's equality binding (`J: Iterator<Item = I.Item>`) made `chain` one. The
  family is closed; what it does not do is *infer*, since an equality is
  compared rather than unified
- a supertrait is resolved in its own sub-pass, so it may name a trait declared
  *later* in the file — unlike a type-parameter bound, which is still resolved
  in the declare pass and so stays order-sensitive. The two spellings of "a
  bound" therefore differ in one respect, for an implementation reason rather
  than a design one
- a trait method's `where` clause is discharged at every call *through the
  trait*, but an impl overriding a defaulted method is neither made to restate
  the clause nor stopped from adding a stronger one: conformance compares
  signatures with `types_equal`, and a signature carries no bounds. An
  override's own bounds are checked where it is called *concretely*, so the gap
  is a strengthened override reached through a bound or a `dyn`
- a `where` predicate naming nothing is silently dropped on a `fun` or an
  `impl` (it simply matches no type parameter). Only a trait method's clause
  reports it, because there the candidates are few enough to name
- an impl's own type-param bounds are checked at selection (milestone 20), but
  *coherence* is deliberately blind to them: `impl<T: Ord> Ord for Option<T>`
  and an `impl Ord for Option<Widget>` conflict even though no receiver could
  ever satisfy both. Disjointness would have to be decidable to do better
- a self-referential blanket impl (`impl<T: Foo> Foo for T`) is cut off by
  `IMPL_BOUND_MAX_DEPTH` rather than diagnosed, so it silently fails to apply
  and the receiver reports "no method named 'foo'"
- two impls of one trait for one type can still coexist in a program, as long
  as no single module sees both — and if two such modules instantiate the same
  *generic* at the same type, `mono_request` memoises one copy and whichever
  requested it first decides the body. Keying instances on the visible set as
  well would fix it, at the cost of a copy per module. Non-generic bodies are
  not affected: since milestone 21 they compile against their own module's
  impl set, which no requester can change
- an `@intrinsic` cannot be used as a value (it is an opcode, so there is no
  body for a global slot to address) — reported at codegen, unlike the
  `@native` beside it which is fully first-class
- `@native`/`@intrinsic` on a *trait*-declaration method is still rejected — its
  default body is generic over `Self`, with no concrete C body to bind. An impl
  method may be native as of milestone 39, and since milestone 40 the standard
  library uses it: the primitive modules spell their operations as methods
  (`s.len()`, `xs.push(v)`, `c.code()`). What stays a free function is the
  design-forced exceptions — the `pad_*`/`float` lang items, the
  `join`/`concat`/`from_chars` builders (receiver is a collection), `print`
  (general over any `T`), `array`'s private `pop_last`, and the length
  `std::cmp` needs but cannot reach as a method without closing the
  `array → option → cmp` cycle (a method needs its impl visible; a free
  `@intrinsic` does not)
- shipping an inherent method on a primitive widely (`impl string`, `impl<T> [T]`,
  `impl char` since milestone 40, and a *second* `impl string` in `std::text`
  since milestone 41) means a program importing that module cannot add its own
  inherent method of the same name. The `Display`/`Ord`-for-containers cost, one
  level over. **Milestone 68 gave it the missing diagnostic** — it is now an
  error where the impl is written rather than a silent first-wins shadowing — so
  what remains is the name budget itself, not the silence. Note this is only a
  problem *across* names: two std impls for `string` coexist fine because their
  names are disjoint, which is what milestone 41 relies on and what milestone 68
  turned from a convention into a rule
- a native's C signature is not checked against its ducktape one — the registry
  knows only "n values in, one out", so a mismatch is a std bug that the
  checker cannot catch
- an array grows but never shrinks its buffer: `pop`, `truncate` and `clear`
  leave the capacity where it got to, and it is released only when the array is
  collected. `reserve` is the only way to move it, and it only goes up — there
  is no `shrink_to_fit`
- there is no `insert` or `remove` at an index. Both are the bulk move
  milestone 106's family is made of (`memmove` rather than `memcpy`), and
  neither has been wanted yet
- **memory exhaustion is not recoverable.** Since milestone 106 it is at least
  *defined* — `heap_alloc` prints `out of memory` and exits — but a program
  cannot observe it, because there is no unwinding to observe it with. It is
  the same shape as a panic, minus the frames
- `for x in xs` re-reads the length each iteration, so pushing to the array
  being iterated extends the loop instead of iterating a snapshot. There is no
  borrow checker to forbid it, and nothing diagnoses it
- `std::array` is no longer a leaf: `pop` returns an `Option`, so importing any
  of it reaches `std::option` and, transitively, every impl `std::fmt` and
  `std::cmp` ship. A program that wanted `push` and its own `impl Display for
  int` cannot have both. Milestones 25 and 26 lengthened that chain — `std::cmp`
  now reaches `std::string` and `std::char`, and `std::char` reaches
  `std::panic` — but not its cost, since none of the three ships an impl to
  inherit
- `std::text::find` slices and interns a candidate substring at every position,
  so `find`/`contains`/`split` are O(*n·m*) allocations. There is no `byte_at`,
  so a window has to be cut out to be compared; a real substring search would
  need one, which is the byte-level reader `std::char` declined to offer
- `std::text::parse_int` wraps on overflow rather than failing: `acc * 10 + d`
  is an ordinary `int` multiply, and detecting the wrap needs a widening multiply
  or a divide-back check the module has not been given a reason to write. So the
  `Option` it returns distinguishes "not a number" from a number, but not a
  number too large from one that fits
- `string` ordering is raw bytes: no locale, no case-insensitive compare, no
  Unicode normalisation, so `"Zebra"` sorts before `"apple"` and two strings
  that are canonically equivalent are simply different. Since milestone 26 a
  case-insensitive compare *is* expressible — `chars` plus `std::char::to_lower`
  — but only for ASCII, so what is left is a data problem (the case-mapping
  table) rather than a language one
- `std::char`'s classifications and case conversions are **ASCII-only**:
  `is_alpha('é')` is false, `to_upper('é')` is unchanged, and nothing warns.
  Full Unicode case mapping is a table rather than a range test, and there is no
  way to ship a partial one that is not silently wrong for most of the world
- a `string` is guaranteed valid UTF-8 (milestone 101), but a *position* into
  one is still a bare `int` byte offset, so cutting inside a character is a
  runtime error rather than something that cannot be written. An opaque
  `StrPos`, obtainable only from a search or a walk, would make it the latter
  and is roadmap item 3 below. The check itself is not the cost the byte-indexed
  API was avoiding — it is two O(1) tests at `slice` — but the *spelling* is
  still one that lets a program name a position it did not get from the string
- nothing hands over a `string`'s bytes: `len` counts them, `matches_at`
  compares them, and there it stops. A `byte` scalar is the wrong shape for the
  gap (see item 3 below — it buys no packing, and 0..255 would be the only sized
  integer in a language that has none), so what fills it is a packed `Bytes`
  object, and nothing needs one until there is I/O to read
- getting one `char` out of a string once meant building the whole `[char]`,
  since the conversion was the only reader. Fixed in milestone 61: `chars` is a
  lazy walk, so `s.chars().next()` decodes one character and
  `s.chars().take(3)` decodes three. The byte/character question is still
  answered milestone 26's way — the walk holds byte offsets and keeps them
  private, so no *program* ever indexes a string by one
- `Ord` ships for `int`, `float`, `char`, `string`, `Option<T>`, `[T]` and
  `(A, B)` (the last two as of milestone 36), so an array of strings sorts and a
  tuple compares field by field without a per-program impl. As with `Display`,
  shipping them takes the pair away from a program that would write its own, and
  only the two-element tuple is covered — nothing is generic over a tuple's
  arity, so a `(A, B, C)` still has no order
- `Ord` for `float` was IEEE comparison, so `NaN.cmp(x)` answered 0 for every
  `x` and NaN compared equal to everything. Fixed in milestone 42: the impl now
  decides a total order — NaN sorts after every real number and all NaNs are
  equal — with `self != self` as the NaN test, so `max(nan, x)` is NaN whichever
  side the NaN is and a `[float]` with a NaN has a defined sort. What the
  placement ignores is the sign bit and payload: `-NaN` and `+NaN` are one
  order, unlike Rust's `total_cmp`, since nothing has needed to tell them apart.
  `string` never had this problem: every byte string is ordered against every
  other
- a `StringBuf` grows but never shrinks its *buffer*: `b.clear()` drops the
  length to zero so one buffer can be reused across iterations, but the capacity
  it grew to is kept, and released only when the buffer is collected
- a `StringBuf` can be appended to from a `string`, a `char` (milestone 26) or an
  `int`'s digits (`b.push_int(n)`, no `"{n}"` interned to carry them), but not
  from a *slice* of a string, so a `b.push_slice(s, from, to)` that avoids
  interning the window first is the natural next entry; it has not been needed yet
- a panic message can only name the value that caused it where the type
  parameter is bounded: `"{e}"` needs `E: Display`, and `Option`/`Result`'s
  `unwrap` are not bounded, so their messages stay fixed. `expect` is the way
  round it
- formatting is the corner of std the compiler knows by name, and since
  milestone 35 that is five names, not one: `Display` (captured in
  `tc_register_trait`) plus `pad_start`/`pad_end`/`pad_center`/`float` (captured
  in `tc_register_fun`), all keyed by module. Every other std item is anonymous
  to the compiler, so these are the exceptions to "the std/not-std difference is
  one branch in `mod_parse`" — each forced by the same rule, that a construct the
  compiler desugars (interpolation, a spec) cannot have its meaning depend on
  what the user happened to import
- `!` is refused in the positions that ask a **structural** question rather
  than a flow one: `if panic("x") { }` wants a `bool`, `for x in panic("x")`
  wants an `Iterator`, and `r?` inside a `-> !` function wants the same enum
  back. Each reports a plain mismatch. Rust accepts all three; nothing is lost
  by refusing them, since the code below is unreachable either way
- a panic does not unwind — no `catch`, and no way for a program to observe one
  and keep running. `!` is only a *type*; the runtime behaviour behind it is
  "print the frames and stop"
- a trait object cannot be made from the abstract `Self` of a default body:
  `check_coerce_dyn` refuses a `TY_TRAIT`, so `self` inside a default body
  can't be handed on as a `dyn Trait` even when the trait is object-safe
- an impl that **overrides a provided method the vtable excludes** is bypassed
  through a trait object: nothing outside the table can find it, so `dyn
  Iterator`'s `map` is `Iterator::map` even for a type that wrote its own
  (milestone 56). Rust behaves the same way, and it is confined to exactly the
  methods object safety could not carry
- a trait's type arguments are never *inferred* at the place the trait is
  named: an impl head, a bound and a `dyn` each write them out, and a bare
  `into` is an arity error rather than a request to work it out. The one
  exception is a `dyn` whose argument is an unsolved unknown, where the impl
  decides — and that is ambiguous the moment two impls answer
- **a bare method call on a type implementing one generic trait twice** picks
  by the *expected* type, and by first-registered-impl when there is none. So
  `print(c.into())` is not the same question as `var f: Fahrenheit =
  c.into()`, and only the second has an answer *as a bare call*. Since milestone
  31 the trait-qualified spelling `into::<Fahrenheit>::into(c)` settles it
  explicitly, so the gap is now only the *bare* form's — a value context with no
  expected type has a way out, it is just not `c.into()`. Since milestone 29 the
  expected type also *pins* an impl parameter the receiver cannot reach, which
  sharpens the bare failure rather than removing it: where that was the only way
  to pin it, no impl applies at all and the diagnostic is "no method named 'into'"
- impl selection reads an **argument** type only through the qualified
  spelling: `Meters::from(x)` picks by `x` (milestone 30), and the receiver side
  `into::<U>::into(x)` picks by `x`'s type (milestone 31), but only through those
  written forms. The argument (or receiver) must type hint-free to do so, so
  `Meters::from(None)` — where the argument is what would choose the impl yet
  needs one chosen to type — cannot be disambiguated
- a bound may name an earlier type parameter of the same list
  (`<T, U: Into<T>>`) but not a later one — bounds resolve left to right, so a
  forward reference is "unknown type: T" rather than a second pass
- a *generic* trait's parameters cannot themselves be pinned by the receiver
  in an impl head: `impl<T> Into<T> for S` type-checks, but nothing solves `T`
  from an `S`, so the impl applies only where a bound, a `dyn`, or (since
  milestone 29) the *expected type* names the argument. The last is what makes
  the `From`/`into` blanket work, and it is the only one that is inference
  rather than a written-down reference
- the slot spaces are two bytes wide, so 65536 functions/structs/enums/vtables
  is a hard program-wide ceiling (reported, not silently truncated) — and every
  instantiation of a generic and every (trait, type) pair coerced spends one,
  so it is reached by *use* rather than by how much is written
- an unsupported construct inside a trait method is reported at the coercion
  site that builds the vtable, since that is where the body is first demanded
  — the same "only seen where it is instantiated" limitation generics have
- a **module qualifier is not re-exportable**: `pub use a;` binds `a` for the
  importer and hands nothing on, because a qualifier is not an item. Milestone
  100 globbed the items but left this — a facade can re-export every type below
  it and still cannot offer the *module* the types came from
- `pub` is ignored on the `impl` keyword and rejected on a method or a struct
  field (see above). Visibility exists at three grains and they do not compose:
  an item takes `pub`, a field takes `pub`, a `mod` takes `pub` — and a *method*
  takes nothing, so it is as visible as its impl however private the path to it
- module dedup is lexical, so one file reached by two different spellings
  (a symlink, say) would load twice and collide
- a bytecode image is structurally validated (bounds, indices, counts) but the
  code itself is not verified: an image with a plausible header and nonsense
  instructions can still crash the VM
- an image carries no source, so a runtime error in one reports function names
  with no line information
- `make sanitize` is a separate run, not part of `make test`: a sanitizer
  finding is not a test failure, so the suite stays green while the binary is
  doing something undefined. Nothing enforces that it is run before a commit
