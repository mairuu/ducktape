# ducktape — completed milestones 76 through 99

Split out of `references/roadmap.md` so the live roadmap stays greppable, the
same way `done-through-m54.md` and `done-m55-m75.md` were. This is history:
nothing here is a plan. The roadmap keeps milestone 100 onward, the "Next" list
and the open warts.

Each entry names the commit that holds the full write-up; `git log` is the home
for a milestone's story, and these are the pointers into it.

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
