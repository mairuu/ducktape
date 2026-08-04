# ducktape — roadmap

## Done

Milestones **through 54** are in `history/done-through-m54.md` and **55–75** in
`history/done-m55-m75.md`, split out to keep this file small. Everything from
76 on is below.

- **76. An operator's result is its own type: the wart's remainder was two
  questions, not one.** `std::ops` grows an `Output` associated type, so a dot
  product `V2 * V2 -> Float` and a reversed `impl Mul<V2> for Float` have
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
    entries by name, so `T: Add<Output = T> + Mul<Output = Int>` merged into one
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
  `Item = Int` impl segfaulted at run. Remainder: none open. Full write-up in
  the commit body.

- **79. A trait object's arguments unify** (`7d115b7`) — `fun pull<T>(s: dyn Src<T>)` takes
  a `dyn Src<Int>` and solves `T`, and `dyn Bag<Item = T>` does the same one
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
  Remainder: the compound *bitwise* operators still have no spelling, and that
  is now a scanning question rather than a design one.

- **84. A binding whose failure leaves** (`7e92800`) — `var Shape::Rect(w, h) = s else {
  panic("not a rect"); };`, Rust's `let else`: the pattern is refutable and the
  success case stays at statement level, so `w` and `h` are ordinary locals
  below it rather than names nested inside an `if var`. Design:
  `language.md` "`var ... else`" + the `Never` paragraph in `std::panic`,
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
  image change. Remainder: a `-> Never` body that falls through is still
  accepted (milestone 85), which is why the trap stays under the `else`.

- **85. A promise nothing was asked to keep** (`f9ca510`) — `fun evil() -> Never { }`
  compiled, handed its `Unit` to whatever the caller declared, and crashed the
  VM on an assertion. Design: `language.md` "`std::panic`" + two "not yet
  implemented" rows, `architecture.md` (inference intro), `runtime.md`
  ("Destructuring a `var`"). The finding: `Never` was not under-checked, it was
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
  `break` in it types `!`, so `fun serve() -> Never { loop { } }` compiles.
  Design: `language.md` "Divergence and `Never`" + the expression list + two
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
  there are none; it types `Int` because its breaks are its only exits and they
  all say `Int` — so the join **replaces** `ExprLoop.has_break` rather than
  joining it, and `NULL` after the body is the divergence case with nothing
  special written for it. Every break is a *sibling*, milestone 85's rule
  applied to however many a loop has instead of to two arms, which is why a
  `break panic("x")` is evidence for nothing and a loop whose breaks all diverge
  is still `!`. The refusal in a `while`/`for` is about the **other exit**, not
  the break: finishing carries no value, so there is nothing for one to join
  with. Codegen needed no opcode and no image change — `OP_SLIDE` already
  removes n values beneath the top, which is exactly a value evaluated before
  the body locals it reads and outliving them. Probing turned up the real cost:
  a `loop` was not on `parse_block`'s tail-promotion list, so `fun f() -> Int {
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
  inherent impls land on *interned* types, `[T]`/`Char`/`String`/`StringBuf`),
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
  Left over: no label on a plain block, and no `unused_label` lint.

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
milestone 75: `V2 * Float`. What it needed from the language was two things
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
   has `push`/`pop`), a growable text buffer (milestone 24, so a `String` can be
   *built* rather than concatenated), and string ordering (milestone 25, so
   `impl Ord for String` exists and text sorts). What is left is breadth, and
   each piece is one registry entry plus a decision about the type it needs.
   Every piece with a design question behind it is now done — the last open one,
   padding, is milestone 34 (below).

   (**An iterator source** was the one piece the iterator work had left
   pointing at itself, and is now milestone 60: `xs.iter()` and
   `(0..n).iter()`. The design question it recorded — what an array iterator
   holds, when the language has no borrow to hold — was answered "the array,
   and nothing snapshotted", which is what `for x in xs` re-reading the length
   each turn already said. **A `String`'s characters as a walk** is the next of
   these, milestone 61, and it answered the same question the other way for the
   same kind of reason: a String is immutable, so its iterator snapshots. What
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
trait-qualified `Into::<Fahrenheit>::into(c)` names the trait so the receiver
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
   no static type, so `a == b` works on any value — Int, struct, generic `T` —
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
   with it; its one live motivation, the `Ord for Float` NaN wart, is an
   ordering bug fixable on its own terms and does not need the trait hierarchy
   to address — and milestone 63 showed the other way out of it, since
   `std::hash` simply declines to implement `Hash for Float` at all.

Not on the roadmap: the **REPL** is a side feature, not a milestone — it lives
on the `feature/repl` branch (`--repl`, incremental compilation over one module
via `Module.decl_base`) and is not part of the main line.

## Known warts to clean up opportunistically

- **one mistake, two diagnostics**, in two places, and the same cause in both:
  a bare-path lookup introduces an inference variable nothing goes on to solve.
  An associated function written where a *pattern* expects a variant reports
  "expected an enum variant in this pattern" **and** an unsolved type parameter
  (milestone 70; a struct has always done this too). The bare
  `it.fold<Int>(0, f)` parses as comparisons, so it reports an unknown *field*
  and then usually an undefined variable for the type name (milestone 71); a
  note on the first names the turbofish, which is as far as that site can see
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
- no compound **bitwise** assignment (`&=`, `<<=`). What it would mean is no
  longer open — milestone 83 settled `a op= b` as `a = a op b` and added `%=` on
  that basis — so what is missing is the spelling, and the scanning is the part
  that stopped it: `>>=` sits against `a > > b` versus `a >> b`, the one place
  in the grammar where whitespace already changes a parse (milestone 65). The
  checker side is a second branch beside `resolve_arith_op` (bitwise unifies at
  `Int` rather than asking a trait), and codegen already has the opcodes
- no unsigned integer type, so `>>>` stands in for one and a bit pattern with
  the top bit set prints negative
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
- `std::ops` ships `impl Add for Int` (and the other eleven primitive impls), so
  a program can no longer write its own — the `Display`/`Ord`-for-primitives
  cost, one module over, and unavoidable for the same reason: without them a
  bounded `T: Add` could not instantiate at a number
- `Display` ships for `[T]`, `(A, B)`, `Option<T>` and `Result<T, E>`, which
  means a program can no longer write its own for those: naming the trait makes
  the std impl visible and coherence rejects the pair. A tuple of any other
  arity has no impl, since nothing can be generic over a tuple's length
- `std::convert`'s blanket `impl<T, U: From<T>> Into<U> for T` is the same wart
  taken to its limit: it applies to *every* self type, so importing the module
  takes `Into` impls away from a program entirely. That is Rust's rule and the
  price of the free direction being free, but it is the widest thing coherence's
  blindness to bounds has cost so far
- a unit struct's name cannot be bound as a variable any more: `var Marker =
  7;` is a struct pattern against an `Int`, and the diagnostic ("expected
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
- a label prefixes a loop and nothing else (milestone 96), so Rust's
  `'a: { break 'a v; }` has no spelling: a plain block still has only its tail
  expression to give a value. Nor is there an `unused_label` lint — a label
  nothing names costs a reader the same as an unused binding, and milestone 92's
  machinery is the shape it would take
- a lint's level is **all-or-nothing over a compile**: `-W` (milestone 97) takes
  no module path, so one module cannot be held to a level the rest is not, and
  the only per-declaration control is `@allow`, which only goes downwards. Nor
  is there a level that *beats* an `@allow` — Rust's `forbid` — so a build that
  must not be silenced anywhere has to grep for the attribute
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
- a **write-only** binding is not reported: `var n = 0; n = 5;` with no read
  resolves the assignment target through the same lookup a read uses, so it
  counts as named. Rust splits this out as its own lint (`unused_assignments`);
  telling the two apart here means knowing the *context* of the lookup, which
  only `EXPR_ASSIGN` with a plain `=` and a single-segment path target has
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
- `Point::new` vs `Point::<Int>::new`: expression paths require turbofish
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
- shipping an inherent method on a primitive widely (`impl String`, `impl<T> [T]`,
  `impl Char` since milestone 40, and a *second* `impl String` in `std::text`
  since milestone 41) means a program importing that module cannot add its own
  inherent method of the same name. The `Display`/`Ord`-for-containers cost, one
  level over. **Milestone 68 gave it the missing diagnostic** — it is now an
  error where the impl is written rather than a silent first-wins shadowing — so
  what remains is the name budget itself, not the silence. Note this is only a
  problem *across* names: two std impls for `String` coexist fine because their
  names are disjoint, which is what milestone 41 relies on and what milestone 68
  turned from a convention into a rule
- a native's C signature is not checked against its ducktape one — the registry
  knows only "n values in, one out", so a mismatch is a std bug that the
  checker cannot catch
- an array grows but never shrinks its buffer: `pop` and `clear` leave the
  capacity where it got to, and it is released only when the array is collected
- `for x in xs` re-reads the length each iteration, so pushing to the array
  being iterated extends the loop instead of iterating a snapshot. There is no
  borrow checker to forbid it, and nothing diagnoses it
- `std::array` is no longer a leaf: `pop` returns an `Option`, so importing any
  of it reaches `std::option` and, transitively, every impl `std::fmt` and
  `std::cmp` ship. A program that wanted `push` and its own `impl Display for
  Int` cannot have both. Milestones 25 and 26 lengthened that chain — `std::cmp`
  now reaches `std::string` and `std::char`, and `std::char` reaches
  `std::panic` — but not its cost, since none of the three ships an impl to
  inherit
- `std::text::find` slices and interns a candidate substring at every position,
  so `find`/`contains`/`split` are O(*n·m*) allocations. There is no `byte_at`,
  so a window has to be cut out to be compared; a real substring search would
  need one, which is the byte-level reader `std::char` declined to offer
- `std::text::parse_int` wraps on overflow rather than failing: `acc * 10 + d`
  is an ordinary `Int` multiply, and detecting the wrap needs a widening multiply
  or a divide-back check the module has not been given a reason to write. So the
  `Option` it returns distinguishes "not a number" from a number, but not a
  number too large from one that fits
- `String` ordering is raw bytes: no locale, no case-insensitive compare, no
  Unicode normalisation, so `"Zebra"` sorts before `"apple"` and two strings
  that are canonically equivalent are simply different. Since milestone 26 a
  case-insensitive compare *is* expressible — `chars` plus `std::char::to_lower`
  — but only for ASCII, so what is left is a data problem (the case-mapping
  table) rather than a language one
- `std::char`'s classifications and case conversions are **ASCII-only**:
  `is_alpha('é')` is false, `to_upper('é')` is unchanged, and nothing warns.
  Full Unicode case mapping is a table rather than a range test, and there is no
  way to ship a partial one that is not silently wrong for most of the world
- a `String` is a byte string, not guaranteed valid UTF-8: `slice` cuts at byte
  offsets, so it can halve a multi-byte sequence, and walking the result is a
  runtime error. Making the type carry the guarantee would mean validating every
  `slice`, which is the cost the byte-indexed API exists to avoid. Since
  milestone 61 the walk is lazy, so the error belongs to the character that
  reaches the bad bytes rather than to `chars()` — a pipeline that stops short
  of them succeeds, which is the ordinary meaning of laziness and not a
  weakening of the check
- getting one `Char` out of a String once meant building the whole `[Char]`,
  since the conversion was the only reader. Fixed in milestone 61: `chars` is a
  lazy walk, so `s.chars().next()` decodes one character and
  `s.chars().take(3)` decodes three. The byte/character question is still
  answered milestone 26's way — the walk holds byte offsets and keeps them
  private, so no *program* ever indexes a String by one
- `Ord` ships for `Int`, `Float`, `Char`, `String`, `Option<T>`, `[T]` and
  `(A, B)` (the last two as of milestone 36), so an array of strings sorts and a
  tuple compares field by field without a per-program impl. As with `Display`,
  shipping them takes the pair away from a program that would write its own, and
  only the two-element tuple is covered — nothing is generic over a tuple's
  arity, so a `(A, B, C)` still has no order
- `Ord` for `Float` was IEEE comparison, so `NaN.cmp(x)` answered 0 for every
  `x` and NaN compared equal to everything. Fixed in milestone 42: the impl now
  decides a total order — NaN sorts after every real number and all NaNs are
  equal — with `self != self` as the NaN test, so `max(nan, x)` is NaN whichever
  side the NaN is and a `[Float]` with a NaN has a defined sort. What the
  placement ignores is the sign bit and payload: `-NaN` and `+NaN` are one
  order, unlike Rust's `total_cmp`, since nothing has needed to tell them apart.
  `String` never had this problem: every byte string is ordered against every
  other
- a `StringBuf` grows but never shrinks its *buffer*: `b.clear()` drops the
  length to zero so one buffer can be reused across iterations, but the capacity
  it grew to is kept, and released only when the buffer is collected
- a `StringBuf` can be appended to from a `String`, a `Char` (milestone 26) or an
  `Int`'s digits (`b.push_int(n)`, no `"{n}"` interned to carry them), but not
  from a *slice* of a String, so a `b.push_slice(s, from, to)` that avoids
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
- `Never` is refused in the positions that ask a **structural** question rather
  than a flow one: `if panic("x") { }` wants a `Bool`, `for x in panic("x")`
  wants an `Iterator`, and `r?` inside a `-> Never` function wants the same enum
  back. Each reports a plain mismatch. Rust accepts all three; nothing is lost
  by refusing them, since the code below is unreachable either way
- a panic does not unwind — no `catch`, and no way for a program to observe one
  and keep running. `Never` is only a *type*; the runtime behaviour behind it is
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
  `Into` is an arity error rather than a request to work it out. The one
  exception is a `dyn` whose argument is an unsolved unknown, where the impl
  decides — and that is ambiguous the moment two impls answer
- **a bare method call on a type implementing one generic trait twice** picks
  by the *expected* type, and by first-registered-impl when there is none. So
  `print(c.into())` is not the same question as `var f: Fahrenheit =
  c.into()`, and only the second has an answer *as a bare call*. Since milestone
  31 the trait-qualified spelling `Into::<Fahrenheit>::into(c)` settles it
  explicitly, so the gap is now only the *bare* form's — a value context with no
  expected type has a way out, it is just not `c.into()`. Since milestone 29 the
  expected type also *pins* an impl parameter the receiver cannot reach, which
  sharpens the bare failure rather than removing it: where that was the only way
  to pin it, no impl applies at all and the diagnostic is "no method named 'into'"
- impl selection reads an **argument** type only through the qualified
  spelling: `Meters::from(x)` picks by `x` (milestone 30), and the receiver side
  `Into::<U>::into(x)` picks by `x`'s type (milestone 31), but only through those
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
  the `From`/`Into` blanket work, and it is the only one that is inference
  rather than a written-down reference
- the slot spaces are two bytes wide, so 65536 functions/structs/enums/vtables
  is a hard program-wide ceiling (reported, not silently truncated) — and every
  instantiation of a generic and every (trait, type) pair coerced spends one,
  so it is reached by *use* rather than by how much is written
- an unsupported construct inside a trait method is reported at the coercion
  site that builds the vtable, since that is where the body is first demanded
  — the same "only seen where it is instantiated" limitation generics have
- no glob `use a::*`. Module-qualified paths exist as of milestone 33 (`use
  a::b;` binds `b`, then `b::thing`), but a *glob* still has no spelling, and a
  `pub use` re-export names one item at a time — a façade module still lists
  them, and a qualifier is not re-exportable (it is not an item). Milestone 95
  made this load-bearing rather than merely tedious: `std::collections`'s
  children are private, so its `pub use` lines are the *only* path to `HashMap`,
  and a type added below the façade is reachable from nowhere with nothing
  saying so
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
