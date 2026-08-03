# ducktape — roadmap

## Done

Milestones **through 54** are in `history/done-through-m54.md`, split out to
keep this file small. Everything from 55 on is below.

- **Arithmetic operators over `std::ops` (milestone 55)** — `a + b` on a
  non-numeric type is the call `a.add(b)`, and `sum`/`product` are combinators.
  Design: `architecture.md` "Arithmetic operators and `std::ops`", `language.md`
  operators / `std::ops`.

  The observation is milestone 38's, one operator family over: **once a trait
  decides what an operator means, the operator is not an operation on its
  operands — it is a method call.** `+`/`-`/`*`/`/`/`%` reach `Add`/`Sub`/`Mul`/
  `Div`/`Rem` and unary `-` reaches `Neg`, six traits of one method each in a
  new `std::ops`. Two numeric operands still keep the opcode — no import, no
  frame, no impl lookup — exactly as `1 < 2` keeps `OP_LT` and `"{1}"` keeps the
  VM's own rendering.

  Three things follow, and they are the whole milestone:
  - **The rewrite *replaces* the node rather than reshaping it, and that is the
    only structural difference from `Ord`'s.** `a.cmp(b)` is an Int the outer
    `< 0` still consumes, so milestone 38 could keep the `EXPR_BINARY` node and
    swap its children. `a.add(b)` is the whole answer — its type *is* the
    operator's type — so `rewrite_ops_call` does `*expr = *call` once
    `resolve_method_call_typed` has typed it against the already-known receiver.
    Either way the operator has stopped existing before codegen, so **the
    opcodes, the VM and the image format did not change at all**.
  - **Six traits, one mechanism.** They differ only by row: `OpsTrait` indexes
    `TypeChecker.ops_traits[]` and `ops_trait_table` holds the three strings
    that vary — the `@lang` marker (which *is* the method name, since a marker
    names the operation rather than the trait), the trait as a bound would spell
    it, and the operator as written. So the checker's diff is one table, one
    gate (`ops_satisfied`, the mirror of `ord_satisfied`), and two call sites.
  - **The traits are homogeneous, and that is forced rather than chosen.** Rust
    writes `Add<Rhs = Self>` with an associated `Output`; ducktape cannot,
    because a generic trait's parameters are never *inferred* where the trait is
    named — so a heterogeneous `V2 * Float` would need the operator to select an
    impl by its right operand, which exists only through the written
    `Meters::from(x)` / `Into::<U>::into(x)` spellings. An operator has neither,
    so a heterogeneous impl would type-check and then fail to be selected. The
    honest shape is the one the language can check.
    **Superseded by milestone 75**, which found the "forced" in this paragraph
    to be an inference argument standing in for a knowledge one: the operator
    has both operand types and writes the argument down rather than inferring
    it. The traits now take `Rhs = Self`. What survives is the sentence about
    `Output`, which really is blocked.

  The payoff is `it.sum()` and `it.product()`, which fall straight out of
  milestone 53: `where Self.Item: Add` is the same clause `max` needed, one
  trait over. Both return an `Option` for the reason `max` does — there is no
  way to write "the identity element of `T`" — and both are excluded from a
  `dyn Iterator` vtable by the same object-safety condition, at no cost.
  `impl Add for String` is what keeps them from being numeric-only.

  The impls `std::ops` ships for `Int`/`Float`/`String` are *not* what makes
  `1 + 2` work; they exist so a bounded `T: Add` can instantiate at a primitive,
  and each body (`return self + other;`) is the built-in path, so it costs a
  frame and nothing else. That is the same asymmetry `std::fmt`'s four `Display`
  impls carry — and here it is not a wart, because an operator's built-in path
  is decided by the operand types, and an impl for a primitive is the only place
  those types *are* primitive.

  `std::ops` joins the prelude, binding all six names rather than only being
  captured: unlike `Display` and `Ord`, whose bounds a program can often leave
  to a call, a generic that does arithmetic must *write* `T: Add`, so the
  diagnostic asking for it has to be followable without an import. The cost is
  the documented one — a program can no longer write its own `impl Add for Int`.

  Two existing tests changed their premise rather than their expectation, which
  is the clearest measure of what moved: `tests/fail/generic_add.dt` used to
  assert "there is no operator overloading" and now asks for the bound, and
  `tests/fail/unary_not_numeric.dt` used to say `-` requires a number and now
  reports a missing `impl Neg`.

- **A trait object satisfies its own bound (milestone 56)** — `first(d)` where
  `d: dyn Iterator<Item = Int>` and `first` takes an `I: Iterator` compiles.
  Design: `architecture.md` "Trait objects", `language.md` "Trait objects".

  The observation the milestone turns on was already written down, in milestone
  13's own words: **"a trait object is a bound whose witness travels with the
  value instead of being resolved."** That sentence describes a *coercion* there.
  Read in the other direction it describes a *bound*, and the code had never
  taken it that way — `impl_index_implements` searched the impl index for a
  `dyn Iterator`, found no `impl Iterator for dyn Iterator`, and reported that
  the trait object of `Iterator` does not implement `Iterator`.

  So the fix is a third early return next to the two that were already there.
  That function answers in registers: a concrete type searches the index, a
  `TY_GENERIC` reads the parameter's own declared bounds, a `T.Assoc` over one
  reads its `where` clauses. A `TY_DYN` is the most direct of the four — it has
  no impl and was promised nothing, because it *is* the trait — so the answer is
  `types_equal(dyn.trait, trait_ref)`, which covers the trait's type arguments
  for free since trait references are interned.

  Two things follow, and the second is the whole design question:
  - **The associated bindings come off the value, not the index.**
    `impl_index_assoc_type` reads a `TY_DYN`'s own table, where the coercion left
    nothing behind. Because `subst_apply` and `infer_apply` both project through
    that one function, this is what collapses `I.Item` to `Int` everywhere at
    once — a `for` loop's variable, `collect`'s element type, and the
    `where I.Item: Ord` a combinator carries.
  - **The object-safe subset governs the vtable, not the bound.** This is the
    question the milestone was recorded as blocked on ("a trait object can only
    witness the object-safe subset"), and the answer is that it never had to
    bite. A method reached on a `T: Trait` receiver instantiated at `dyn Trait`
    splits: a *required* one goes through the vtable, and object safety already
    guarantees it is dispatchable, or `dyn Trait` would not have been writable.
    A *provided* one is a generic function over `Self`, so monomorphising its
    body at `Self = dyn Trait` compiles it like any other instance and the
    `self.next()` calls inside dispatch through the table on their own.
    Dispatchability never enters, because nothing goes through a slot.

  That last one was also a live miscompile: codegen's `TY_DYN` branch was
  unconditional, so `it.map(f)` under `I = dyn Iterator` emitted `OP_DYN_METHOD`
  into the slot object safety deliberately leaves empty and the VM pushed a NULL
  function — a segfault, reproduced before the fix. Codegen now branches on
  `undispatchable` and takes `TraitMethodDef.default_impl` instead.

  **Two special cases died, which is the measure of whether the rule is the
  right one.** `resolve_for_iterator` had carried a second disjunct admitting a
  `dyn Iterator` by trait-def identity (milestone 47) precisely because the
  bound would not; it now gates on one question. And
  `check_trait_method_call`'s rejection of an excluded method through a `TY_DYN`
  receiver went too — keeping it would have made `d.map(f)` an error while
  `f(d)`, handing the same value to a `<I: Iterator>` function whose body is
  `it.map(f)`, compiled the very same code. Its diagnostic had even advised
  "call it on a concrete or bounded receiver", which by then named a workaround
  producing identical output. What is still refused is an *associated function*:
  no receiver means no value to carry, the one undispatchable shape no
  instantiation rescues.

  Two existing `tests/fail` entries became `tests/run` entries, their premises
  inverted rather than their expectations adjusted:
  `dyn_excluded_method.dt` (asserting `d.map(..)` was an error) and
  `dyn_assoc_bound_method.dt` (asserting `d.max()` was, because "whether the
  bound holds is a question about the concrete type ... precisely what the
  coercion throws away"). The premise is true of the vtable and false of the
  bound; the *call site* can read `Item` off the trait object and ask whether it
  is `Ord`. So the predicate is now discharged rather than waived, and
  `tests/fail/dyn_bound_assoc_unsatisfied.dt` is the case where it genuinely
  fails — an error the old blanket rejection used to hide behind a complaint
  about dispatch.

  The one cost worth recording: a concrete impl that *overrides* an excluded
  provided method is bypassed through a trait object, since nothing vtable-less
  can see it — `dyn Iterator`'s `map` is always `Iterator::map`. Rust makes the
  same trade for the same reason, and it is confined to methods the vtable
  cannot carry.

- **`std::assert` (milestone 57)** — `assert`, `assert_with`, `assert_else`,
  `assert_eq`, `assert_ne`. Design: `language.md` "`std::assert`".

  **Zero compiler change, and the first milestone in a while that is only a
  `.dt` file.** Everything it needed had already landed: `Never` and `panic`
  (37), the `Display` bound and interpolation through a type parameter (20),
  closures as parameters (long before). What was left was the decisions, and
  there are three.

  - **An assertion cannot name itself.** Rust's `assert!` is a macro *for this
    reason* — a macro sees the source text `a == b` and can put it in the
    message. A ducktape function sees a `Bool`. So `assert(cond)`'s message is
    the fixed "assertion failed" and no amount of work inside the function can
    improve it; saying more means the caller saying it, which is
    `assert_with`. That is the same split `Option` already has between `unwrap`
    and `expect`, and it is worth noticing it comes from the same absence twice:
    a callee cannot describe its own argument, whether the argument is a value
    it cannot render or an expression it never saw.
  - **Arguments are evaluated, so laziness needs a closure.** `assert_with(ok,
    "index {i} of {n}")` builds that string on every passing call — precisely
    the cost a macro hides. `assert_else` takes a `fun() -> String` instead, and
    the shape is not new: it is `unwrap_or` / `unwrap_or_else`, chosen for the
    same reason a third time. `tests/run/std_assert.dt` pins the difference by
    *observing* it — the closure's body prints, so laziness is an output diff
    rather than a claim.
  - **The bound is on the reporting, not on the comparison.** `assert_eq<T:
    Display>` looks like it needs `Display` to be an equality function, and it
    does not: `==` lowers to `OP_EQ`, which reads no static type at all, so two
    values of a type with no impls compare fine. The bound is bought entirely by
    the failure message — the runtime can render any value, but only to stdout,
    and `panic` takes a `String` no native produces from a structural walk.
    `tests/fail/assert_eq_needs_display.dt` is that seam: a `Widget` the
    comparison would accept and the message cannot name.

  That last point is the milestone's contribution to item 3 below. `assert_eq`
  is the closest thing to a *consumer* of equality std has, and it turns out to
  argue **against** an `Eq` trait rather than for one: what it wanted from the
  language was a way to print, not a way to compare. A hash map keyed by user
  types would still be the real test.

  **It is a module of its own, and would be even at five short functions.**
  `Display` drags in `std::fmt`'s impls, impl visibility is transitive, so
  folding these into `std::panic` would hand a program that only wanted
  `panic("...")` every `impl Display for` in std — and with them coherence's
  refusal to let it write its own. That is `std::cmp`'s argument about where
  `impl Ord for String` belongs, applied again: an import is measured in impls,
  not in code, so the dependency points at the impl-poor module. `assert`
  imports `panic`; the reverse would be the expensive direction.

  Recorded as a limitation rather than fixed: there is **no conditional
  compilation**, so an assertion is ordinary code that always runs and always
  costs. Nothing here is a debug-only check that disappears from a release
  build, and the language has no notion of one to hang it on.

- **Supertraits, and `rev` (milestone 58)** — `trait DoubleEnded: Iterator`, so
  a trait may require another. Design: `architecture.md` "Supertraits",
  `language.md` "Supertraits" / `std::iter` → "`DoubleEnded` and `rev`",
  `runtime.md` "Trait objects".

  The observation the milestone turns on: **a supertrait is not a new kind of
  thing, it is an edge.** Four questions the checker already asked of an
  abstract type — what does it implement, what methods may it name, what
  associated types has it, and what can a vtable carry — were each answered by
  scanning one trait's own list. A supertrait makes each of those scans
  transitive. Nothing else about a trait, an impl, a call or a coercion
  changed, and the closure a trait with *no* supers has is one entry, itself,
  so every reader **dropped** a special case rather than gaining one.

  It says one thing with two readings, and the milestone is only usable because
  both are enforced:
  - **As an obligation**, `impl DoubleEnded for Span` requires
    `impl Iterator for Span`, checked in `tc_check_impl_conformance` against
    the impl's own module.
  - **As an assumption**, a `T: DoubleEnded` bound hands you all of `Iterator`
    — answered off the declaration, without searching for an impl at all.

  The second is only sound because of the first, and that is the whole of why
  the obligation is not a nicety: `impl_index_implements` reads a super
  straight off a `TY_GENERIC`'s bounds, so an `impl DoubleEnded` without an
  `impl Iterator` would be a lie the checker had already believed.

  Three things follow, and the third is the one worth recording:
  - **The pass structure had to grow a third sub-pass.** A signature may
    project through a super (`Self.Item` where `Item` is the super's), so every
    trait's supers must be resolved before *any* signature is — but resolving a
    super needs every trait's name bound. So `resolve_trait_supers` and
    `trait_build_closure` sit between declare and body, and the associated-type
    *names* moved into the declare pass, where they are a straight copy off the
    AST. The result is that a supertrait declared later in the file works, which
    a bound on a type parameter still does not.
  - **`trait_project` has to look past the impl it was handed.** A supertrait's
    associated type is bound by the impl of the trait that *declares* it, so
    `impl DoubleEnded for Rev<I>` says nothing about `Item` and the projection
    used to survive to a signature comparison as an unresolved
    `DoubleEnded.Item`. It now falls back to `impl_index_assoc_type` on the self
    type. That is the one place the feature cost a real edit rather than a
    substituted scan.
  - **`dyn Sub` works, and there is no diamond problem to solve.** This was the
    question the milestone looked blocked on: a flat vtable numbering over a
    closure seems to require that `dyn A`'s and `dyn B`'s tables agree about
    `B`'s methods, which a diamond breaks. They never have to. **A trait object
    always carries the table of the trait it was *written* as, and every
    dispatch on it indexes that same trait's closure** — the language has no
    `dyn A` → `dyn B` coercion, so two tables are never compared. So the vtable
    and the associated-type table both flatten, object safety is decided over
    the closure, and a `dyn DoubleEnded<Item = Int>` is an `Iterator` in every
    sense a bound or a `for` loop can ask about.

  `std::iter` gains `DoubleEnded` (`next_back`, `rev`), the `Rev<I>` adapter and
  its two impls, and forwarding impls on `Map` and `Filter`. Which adapters
  forward is the library's one judgement call, and each refusal has its own
  reason rather than a shared one: `take`/`skip` would need a length to know
  where the far end is, `enumerate`'s index counts from the front, `zip` pairs
  by position, and a `chain` driven from both ends needs its halves to agree on
  where they met — which two independent `done` flags cannot say.

  `DoubleEnded` is deliberately **not** a lang item and not preluded: nothing
  the compiler desugars names it, unlike `Iterator`, which `for` speaks.

  One existing test changed its premise rather than its expectation, which is
  the usual measure: `tests/fail/trait_where_self_bound.dt` asserted that
  `where Self: Ord` was rejected *because ducktape has no supertraits*, and now
  asserts it is rejected because a supertrait belongs on the trait rather than
  on one method — one spelling, not two.

- **Iterator breadth (milestone 59)** — `any`, `all`, `find`, `position`,
  `count`, `for_each`, `take_while`, `skip_while`. Design: `language.md`
  `std::iter` → "Combinators".

  **Zero compiler change**, the second such milestone after 57, and the one the
  roadmap had been describing for four milestones as "breadth with no design
  question behind it". That turned out to be true, which is itself the result
  worth recording: 47–58 spent the design budget, and what is left of the
  iterator direction is `.dt` written against `next()`.

  What the milestone had to *decide* rather than implement is where each thing
  stops, and there are three:
  - **Short-circuiting is part of the meaning, not an optimisation.** `any`
    stops at the first element that passes, `all` at the first that fails,
    `find`/`position` at the first match — and the cursor is left there, so the
    same source may be driven on afterwards. `tests/run/iter_breadth.dt` pins
    that by *observing* it: the source prints what it has been pulled for, so
    stopping early is an output diff rather than a claim — the shape milestone
    57 used for `assert_else`'s laziness. `count` is the exception and says so:
    the answer is only known once the sequence ends.
  - **`take_while` is spent at the first failure and stays spent**, which is
    the whole difference from `filter` (`0,1,2,3,0,1` gives `[0,1,2]` rather
    than `[0,1,2,0,1]`). That needs a `done` flag, and the element that failed
    is gone — pulling it is how the adapter found out, and there is nowhere to
    put it back. Its mirror `skip_while` has the matching seam from the other
    side: the element that *ends* the skipping is the first one yielded, and
    dropping it would silently eat one element of every sequence.
  - **`for_each` takes a `fun(Self.Item) -> Unit` and there is no discard
    coercion.** A shorthand closure *is* its expression, so `|x| f(x)` fits
    only where `f` returns nothing; `|x| { f(x); }` is the way round it, the
    trailing `;` doing the dropping by the ordinary block rule rather than by
    anything bought for this method (`tests/fail/iter_for_each_returns.dt`).

  The one thing the milestone *found* rather than decided: **the object-safety
  partition falls along a line the library already had.** All six consumers sit
  in the `dyn Iterator` vtable and both new adapters are excluded, and the
  reason generalises — an adapter returns a `Self`-shaped type and a consumer
  does not, so what excludes a consumer is instead a type parameter of its own
  (`fold`) or a bound on the element (`max`). Milestone 48 wrote the
  per-method rule without that split in mind; it is what the rule turns out to
  mean once `std::iter` is wide enough to show it.

- **Iterator sources (milestone 60)** — `xs.iter()` and `(0..n).iter()`.
  Design: `language.md` `std::iter` → "Sources".

  The direction milestones 47–59 had been building toward without being able to
  reach: every combinator wraps an iterator, and the two sequence types a
  program actually has were not ones. `for x in xs` and `for i in 0..n` are
  desugarings *over* an array and a range, so neither implemented `Iterator`
  and neither could reach a combinator — every iterator a program could drive
  was one it had written itself. Two seeds close that, and the whole of 47–59
  applies to both.

  Both are ordinary `.dt` in `std::iter`: `ArrayIter<T>` and `RangeIter`, each
  with an `Iterator` and a `DoubleEnded` impl, plus `impl<T> [T] { fun iter }`
  and `impl Range { fun iter }`. The compiler side is ~15 lines.

  - **`Range` becomes a nameable type**, joining `String`/`StringBuf`/`Char` as
    a builtin the compiler knows while every operation on one lives in std:
    `t_range` on the `TypeChecker`, a line in `type_named_builtin`. That is the
    entire language feature, and what it buys is a *self type to write* — an
    `impl Range` — plus a range as a parameter (`fun span(r: Range)`). The
    price is the one every builtin name carries: a program's own `struct Range`
    is now unreachable by name.
  - **The intrinsic tier pays out a second time.** `OP_RANGE_START` already
    existed for the `for` desugaring; `range_start` is milestone 40's
    `array_len` move exactly — an opcode that was emitted but could not be
    *named*. The one new opcode is `OP_RANGE_STOP`, and its shape is the
    observation the milestone turns on: **`a..b` and `a..=b` are two spellings
    of one sequence**, so it answers with the first Int *past* the end and the
    inclusive flag is folded away. `RangeIter` therefore holds two Ints and no
    `Range` — an iterator that remembered which spelling it came from would be
    carrying a distinction its own sequence does not have. Both intrinsics are
    private free functions rather than methods, for `pop_last`'s reason: an
    impl method has no visibility control, so a `Range::stop()` would be public
    API on every range in every program.
  - **What an array iterator holds** is the question the roadmap recorded as
    this milestone's design fork. In a language with borrows `xs.iter()` holds
    one and the borrow checker answers "what if the array changes underneath";
    ducktape has none, so the question has to be *answered* instead — and the
    answer already existed, since `for x in xs` re-reads the length each turn.
    `ArrayIter` snapshots nothing: it carries how far in it has come from
    **each end**, never a remembered end index, so both directions re-read
    `len()`. Growth ahead of the front cursor is yielded, shrinking ends the
    walk early, and no interleaving of the two can read off the end — the
    property the "taken from the back" count buys over the obvious stored
    bound. `tests/run/iter_source_shared.dt` *observes* all three rather than
    claiming them, the shape milestones 57 and 59 used.
  - **Both sources live in `std::iter`, not in `std::array`**, and that is the
    import graph rather than a preference: `collect` needs `push`, so `iter`
    depends on `array` and the dependency cannot also run the other way. A
    module cycle is a compile error, so the placement was decided before it was
    chosen.
  - **`for x in xs` keeps its desugaring.** Routing it through `iter()` would
    make the loop depend on a std impl and cost a frame per element for a shape
    the codegen already has; the two paths agree on semantics, which is the
    point of matching the re-read.

  `DoubleEnded` on both is what makes the milestone worth more than its size:
  `xs.iter().rev()` and `(0..n).iter().rev()` are the reverse walks milestone 58
  built the trait for and had only a hand-written `Countdown` to spend it on.

- **A String's characters as a walk (milestone 61)** — `s.chars()` answers a
  `CharIter` rather than a `[Char]`. Design: `language.md` `std::iter` →
  "Sources", and `std::char` → the String/Char split.

  The third source, and the first whose sequence is not already a sequence of
  *values*: an array's elements and a range's Ints are there to be handed over,
  while a String's characters have to be decoded out of bytes. That is what had
  made the crossing a conversion — one native returning the whole array — and
  it put the pieces in the wrong order: the array was the primitive and the walk
  was a thing you did to it afterwards. It is the other way round.
  `s.chars().collect()` is the array, `s.chars().count()` builds none, and
  `s.chars().take(3)` decodes three characters however long the string is.

  - **What it holds is milestone 60's question with the opposite answer, and the
    difference is the objects' rather than a preference.** `ArrayIter` refuses
    to snapshot because an array is the one heap object a program can grow, so
    it re-reads `len()` every turn; a String is interned and immutable, so
    `CharIter` reads it once. The question milestone 60 had to *answer* (what
    does an iterator hold, in a language with no borrow to hold) does not arise
    here — immutability dissolves it, which is worth recording as the reason
    rather than as a coincidence.
  - **Only one direction needs a native, and the asymmetry is the encoding's.**
    A `Char` *is* a scalar value and a scalar value determines its UTF-8 width,
    so stepping forward is a four-branch range test in ducktape — the same shape
    every `std::char` classification has. Stepping backward cannot be derived
    from any value in hand, because there is no character yet; the only thing
    that says where one begins is the tag UTF-8 puts on its bytes. So
    `string_prev_boundary` is the one genuinely new operation, a bounded scan
    over continuation bytes, and it is what makes `s.chars().rev()` cost what
    the forward walk costs instead of building an array to turn round.
  - **Both natives speak in byte offsets, so both are private** — free functions
    in `std::iter`, `range_start`'s reason exactly (an impl method has no
    visibility control). Milestone 26's rule that there is no `char_at(s, i)`
    survives intact: `CharIter` holds byte offsets because it must, and no
    program can name one.
  - **Placement was forced again, and this time it left a scar.** `std::cmp`
    imports `std::string`, so `std::string` can never import `std::iter` — the
    walk had to live with the other sources, and `std::string`'s `pad_*` lang
    items cannot reach it. What they need is not the characters but *how many*,
    so what they keep is `string_char_count`: one validating pass allocating
    nothing, where the old conversion built an entire array to take its length.
    The trade is visible in the registry — `string_chars` is gone, and the
    counter is what is left of it.
  - **Laziness moves when invalid UTF-8 is reported**, from the conversion to
    the character that reaches it. `s.chars().take(0)` over a string `slice` cut
    in half now answers instead of failing (`tests/run/iter_chars.dt` observes
    it; `tests/fail_run/chars_invalid_utf8.dt` is the same string driven to the
    end). That is what being lazy means rather than a weakened check: nothing is
    claimed about bytes nobody looked at.

  The call sites that changed say what the milestone is: `std::text`'s `trim`
  and `parse_int` now write `.chars().collect()` — they want the characters by
  index from both ends, which is exactly the case the array was always right
  for, and now it is requested rather than assumed.

- **A sorted `[T]` (milestone 62)** — `std::sort`: `sort_by`, `sort`,
  `is_sorted`, `lower_bound`, `binary_search`. Design: `language.md`
  `std::sort`.

  The shape "breadth" takes when a piece needs **nothing** from the language:
  no native, no opcode, no compiler change, one new `.dt` file. Everything it
  uses was already there — a bounded inherent impl block, a closure in a
  parameter, index assignment, `Ord` — so the milestone is entirely its
  decisions, and each one is a promise to a caller rather than a technique.

  - **`sort_by` is the primitive, `sort` the convenience**, milestone 59's
    `max` fork settled the same way: the closure form takes the order as an
    argument, the `Ord` form takes it from the element. The bound sits on a
    separate `impl<T: Ord> [T]` block rather than on each method, because it is
    a property of *which arrays these apply to* — and the diagnostic that comes
    out says so ("an impl exists, but it requires 'P: Ord'"), which is a better
    answer than "no such method" (`tests/fail/sort_needs_ord.dt`).
  - **Stability is the promise that picks the algorithm.** A bottom-up merge
    sort keeps it unconditionally; an in-place quicksort would trade it away
    and take quadratic time on the already-sorted input a program is most
    likely to hand it. The cost is one array of scratch space, which in a
    language where `collect` already copies is a price with a spelling. What
    stability *buys* is sorting by one key and then another, and
    `tests/run/sort.dt` observes that rather than asserting it.
  - **Sorting is in place**, because `[T]` is a reference and the caller is
    holding the array being rearranged. No `sorted()` copy ships, and that is
    milestone 60 paying out somewhere unrelated: `xs.iter().collect()` is the
    copy, so a second method would only be a name for two calls.
  - **The merge cannot ask which array it ended on.** `src`/`dst` are names for
    arrays and the passes swap them, but `==` on two arrays is *structural*
    (milestone 5c-i), so two arrays holding the same elements are equal and
    identity has no spelling in the language at all. The parity of the pass
    count is that missing question, answered by counting instead of asking.
  - **`lower_bound` is the primitive of the two searches**, not the private
    half of `binary_search`: an insertion point exists whether or not the
    element does, so the search is one comparison past it while the reverse
    needs rewriting. It is also what an ordered map would be built on, which is
    the next item and the reason this one is `pub`.
  - **"Found" means `cmp` answers 0, not `==`** — and where the two disagree
    (a struct ordered on one field, a case-insensitive comparator) it is the
    order's answer a search can honour, since the order is what put the
    elements where they are. That is a small argument *for* the deferred `Eq`
    staying deferred: the notion of equality a search needs was already written
    down as `Ord`.
  - **Placement, and what made it possible.** A module of its own rather than
    more of `std::array`, `std::text`'s split one type over: `std::array` is
    what an array cannot say about itself, while an order is a policy on top.
    The edge `sort → cmp` closes no loop only because milestone 36's
    `std::cmp` reaches an array's length through a private `@intrinsic` instead
    of importing `std::array` — a decision made for impl-visibility reasons
    that turns out to be what lets this module exist.

  Two things it found rather than decided. `tests/run/sort_oracle.dt` is the
  first test in the suite that checks an *algorithm* rather than an API: an
  expected-output test cannot reach the lengths where a merge sort's tail run
  and copy-back parity go wrong, so it sorts every length up to 40 and compares
  against an insertion sort written out beside it. And the reason `std::sort`
  is opt-in rather than preluded is a wart it surfaced — a visible std method
  on `[T]` silently wins over a program's own method of that name, with no
  diagnostic (below).

- **A hash map (milestone 63)** — `std::hash` (`Hash`, `Hasher`, `hash_of`) and
  `std::map` (`HashMap<K, V>`, `HashSet<T>`). Two natives, no opcode, no
  compiler change. Design: `language.md` `std::hash` / `std::collections`,
  `runtime.md` "Native functions". **The module is `std::collections` now**
  (`9e821f5`+): it had held a set as well as a map since the day it shipped, so
  `map` named one of its two types. Below this line it is still spelled
  `std::map`, which is what it was called at the time.

  This was the fork the previous milestone left open — a hash map keyed by user
  types was the concrete consumer item 3 said `Eq` was waiting for — and the
  answer is that **it is not one**. `==` in ducktape is a structural primitive
  with no trait behind it, so `k == key` type-checks on a `K` whose only bound
  is `Hash`, and one opcode compares Ints, Strings, structs and tuples alike. A
  map needs equality, takes it from the language, and spends its bound entirely
  on hashing. `Eq` goes on waiting for a different consumer, and the argument
  against it is now stronger than before: the structural `==` also makes the
  `Hash`/`Eq` contract nearly unbreakable, since there is no custom equality for
  a hash to disagree with.

  - **The trait's shape is decided by a gap in the language.** (Closed since,
    by milestone 65 — the shape is kept, and the argument below is what it was
    at the time.) ducktape had no bitwise operator at all — `^` was the
    centre-align token in a format spec, `|` opened a closure, `&` and the
    shifts did not exist — so a mixing function was not awkward to write here,
    it was *unwritable*. The one an implementor
    could write is `h * 31 + x`, which is precisely the weak one: multiply and
    add both carry information upward, so the high bits never reach the low
    bits `%` reads. Hence `fun hash(self, h: Hasher)` rather than
    `fun hash(self) -> Int`: an impl says only *which parts of me matter*, the
    combining lives in one object, and that object reaches C. `Hasher` is to
    `Hash` what `StringBuf` is to building text, and the parallel is exact —
    both exist because the work below them cannot be said above them.
  - **The accumulator needed nothing new.** A `Hasher` is handed to a value
    rather than returned from it, which works for the same reason `xs.push(v)`
    needs no way to spell "by reference": a struct is a heap object behind a
    handle, so a callee assigning to `self.state` assigns to the caller's.
  - **A String's hash is a field read** — `heap_intern` computed it to find the
    string's bucket — so the type whose hash would otherwise be a walk over its
    bytes is the cheapest impl in std rather than the dearest.
  - **No `impl Hash for Float`**, and the contrast with `impl Ord for Float` is
    the point: an order is total and so had to put NaN *somewhere*, while a
    table may decline a key. `NaN != NaN` would make such a key unfindable in a
    table that compares with `==`, and a truncating `as Int` is the only number
    a Float can reach here anyway.
  - **Capacities are primes because they are allowed to be.** (Superseded by
    milestone 66, which switched to powers of two once `&` existed.) Every table
    that indexes with `h & (cap - 1)` is forced to a power of two by the mask;
    with no `&` the reduction is `%`, nothing pushes toward powers of two, and a
    prime is `%`'s natural partner. The sign fold that goes with it is not
    optional — `%` keeps the sign of its left operand, so `(0 - 17) % 5` is
    `-2`, and getting it wrong is an out-of-bounds read rather than a bad
    distribution.
  - **A slot is an enum, not parallel arrays.** `[K]` would have to be *filled*
    at every empty slot and a generic `K` has no default value to fill it with;
    `Slot::Empty` holds nothing, so there is nothing to invent. It is also one
    allocation rather than `n`, since every free slot can share the one `Empty`.
  - **`iter()` holds the slot array rather than the map** — `CharIter`'s choice
    rather than `ArrayIter`'s, and forced rather than chosen: a rehash moves
    every entry, so an index into the old table means nothing in the new one.

  Two things it found rather than decided, both in the test suite. The first is
  a **parser bug it walked straight into**: `self.at < n` did not parse, because
  a `<` after a field or method access was unconditionally read as opening type
  arguments. Fixed ahead of the milestone in its own commit, with the
  disambiguation reading what follows the matching `>`
  (`tests/run/method_type_args.dt`). The second is worse and is a lesson about
  oracles: `tests/run/map_oracle.dt` was written, passed, and was then checked
  against two *deliberately broken* maps — and passed those too. The cause was
  the LCG's low bits, which are degenerate at a power-of-two modulus, so
  `seed % 4` was a two-cycle that drew `get` zero times in four thousand
  operations and `seed % 40` only ever yielded odd keys. Reading the high bits
  instead turned both sabotages into thousands of mismatches. `sort_oracle.dt`
  had the same flaw and is fixed with it — **an oracle that has not been shown
  to fail is not evidence**, which is now the standing rule for this suite.

- **The runtime calls back into ducktape (milestone 64)** — `native_call`, and
  `sort_by` moved into C. Design: `runtime.md` → "Calling ducktape from a
  native".

  Every native until now answered out of C alone, and one kind of native
  cannot: a sort's *order* is a value the program wrote. Milestone 62 put the
  whole merge in ducktape for exactly that reason — the comparator was only
  callable from there. This is the boundary opened in the other direction, and
  what it costs is an invariant rather than any code: **a native may now run
  user code**, which is a much larger statement than the function that needed
  it.

  - **The mechanism is one parameter.** The bytecode loop became
    `run(Vm *, int stop_depth)`, and `OP_RETURN` ends it when `frame_count`
    falls back to `stop_depth` rather than to zero. That is the whole of
    re-entrancy: C pushes a frame, drives that one call to its return, and the
    outer `run` is still suspended underneath with its frames untouched.
    `vm_run` is `run(&vm, 0)` and is the loop it always was. The call is built
    exactly as `OP_CALL` builds one, which is why a comparator may be a closure
    *or* a plain function passed by name — the callee is a `Value`, resolved the
    one way.
  - **A re-entrant interpreter is a re-entrant collector**, and that is the
    consequence with teeth. The old calling convention ("arguments stay on the
    stack, so they are rooted") quietly assumed a native allocates only for
    itself; now the *callee* allocates, so a collection can land between any two
    elements. Anything a native holds must be a root — `native_root` is a push,
    since the stack is the root set — and the negative check is the evidence:
    delete those pushes and `tests/run/sort_callback.dt` under `--gc-stress`
    stops being a wrong answer and becomes a heap-corruption abort.
  - **A pointer into a heap object cannot be held across a callback.** The
    comparator can reach the array it is ordering, and a `push` reallocates
    `items` underneath C. So the sort works on scratch the program cannot reach
    and touches the caller's array only before the first comparison and after
    the last — which turns the mutation question into one visible decision:
    resizing the array mid-sort is *refused* (`tests/fail_run/sort_resize.dt`),
    because the snapshot describes a sequence that no longer exists and half-
    applying it would be worse than saying so. Reading it, or sorting another
    array, is fine.
  - **A failing callback reports itself.** `ctx->unwinding` says the error was
    already printed with the frames that were live, so the VM adds nothing —
    the alternative describes one failure twice, from outside. `OP_CALL` also
    asserts the stack came back level, because a native that returns askew
    corrupts the frame beneath it rather than failing.

  What it proves is bigger than what it buys: milestone 62's tests — stability
  observed, and the oracle over every length to 40 — pass against the C
  unchanged, so the two implementations agree on the promise rather than only on
  the examples.

  **Measured, it is ~3×, and the interesting half of that is what is left.**
  Both implementations in one `-O2` binary, images pre-compiled, best-of-5, with
  an array-building baseline subtracted: 2.9× at n=2 000, 3.3–3.8× at n=10 000,
  2.7× at n=50 000, all with a cheap `|a, b| a - b`. `xs.sort()` gets only
  2.2×, because the `Ord` path's comparator is two interpreted calls (the
  closure, then `cmp`) and moving the merge to C does not touch either. A
  temporary probe doing the comparison in C instead of calling back finished the
  same work in 2ms against the native's 13ms — so **85% of what the native still
  spends is the callback**, and the merge loop was never the expense. The
  general rule that falls out, and the one to apply to the next candidate:
  moving work to C pays in proportion to the interpreted *iterations* it deletes
  minus the callbacks it must still make, which is why a container whose
  per-operation work is a user-written trait method is a much worse candidate
  than a sort.

- **Bitwise operators (milestone 65)** — `& | ^ << >> >>>` and unary `~`, Int
  only. Scanner, parser, checker, two opcode families, no std change. Design:
  `language.md` "Expressions".

  This closes the gap milestone 63 recorded as its sharpest finding: with no
  bitwise operator, anything defined in terms of bits was not awkward to write
  in ducktape but *unwritable*, which is why `std::hash` puts its mixing in C.
  The evidence that it is closed is `tests/run/bitwise_mixer.dt`, which writes
  `hash_mix` out in ducktape constant for constant and checks it against the
  native on a thousand inputs plus the awkward ones. **The native stays**, and
  that is the point rather than a compromise: it is now a choice justified by
  milestone 64's cost model — six operations of pure arithmetic with no callback
  is the shape that pays — instead of the only option. Keeping the ducktape twin
  under test is what stops the optimisation from becoming a black box.

  - **Two spellings were already taken, and neither had to move.** `|` opens a
    closure and `^` is the format spec's centre-align, but both are settled by
    position: a closure is only ever reached from `parse_primary`, so a `|` with
    a left operand is unambiguously the operator, and a format spec is parsed on
    its own path. Dropping the fat arrow first made this *more* delicate rather
    than less — a closure body is now an unbraced expression, so `|x| x | 1` has
    to take the `|` as part of the body — and greedy parsing gets it right.
  - **`>>` cannot be a token.** A nested generic closes with a run of `>`
    (`HashMap<String, Option<Int>>`), and `parse_type` consumes those one at a
    time, so a scanner that fused `>>` would break every type that nests. The
    scanner therefore emits single angle brackets and `parse_shift` fuses
    adjacent ones, which also leaves milestone 63's `looks_like_type_args` scan
    exact. The adjacency test is what separates `a >> b` from `a > > b`, and it
    is the only place in the grammar where whitespace changes a parse.
  - **Precedence is Rust's, not C's**: every bitwise operator binds tighter than
    comparison, so `a & b == c` is `(a & b) == c` rather than C's fifty-year-old
    `a & (b == c)` trap. Shifts stay looser than `+`, which is the half of C's
    ordering worth keeping.
  - **`>>` propagates the sign and `>>>` shifts in zeros**, because `Int` is
    signed and there is no unsigned type to make the distinction for us. The
    milestone's own test is what proves the split earns its keep: transliterating
    splitmix64 with `>>` in place of `>>>` agrees on small positives and
    diverges everywhere else.
  - **A shift count outside 0..63 is a runtime error**, not a masked count.
    Java's masking makes `x << 64` equal `x`, which looks deliberate and is
    almost never meant; an out-of-range index already reports rather than
    guesses.
  - **They are the only binary operators that can drive inference.** Every other
    one has to look at its operands before it knows what it means — `+` might be
    Int, Float, String or a call to `Add` — while a bitwise operator has exactly
    one operand type. So they unify with `Int` rather than testing for it, and
    `|x| x | 1` solves `x` where `|x| x + 1` still cannot.

  And one bug found rather than decided, which is the milestone's real dividend.
  Writing a mixer in ducktape means multiplying constants that overflow by
  design, and UBSan reported it immediately: **the VM was getting the wrapping
  the language promises out of undefined behaviour**, computing `x * y` on
  `int64_t`. Worse, `INT64_MIN / -1` is not merely undefined but raises SIGFPE,
  so `lo / neg` in ordinary ducktape killed the process. Addition, subtraction
  and multiplication now compute in `uint64_t` and convert back — the same
  instruction with the promise actually written down — and the two overflowing
  divisions answer `INT64_MIN` and `0`. Nothing in the suite had reached either,
  because nothing had needed arithmetic to wrap on purpose before.

- **A masked hash table (milestone 66)** — `std::map` swaps prime capacities and
  `%` for powers of two and `h & (cap - 1)`. One `.dt` file, no compiler change.
  Design: `language.md` `std::collections`.

  The first thing milestone 65 paid for, and the smallest kind of milestone
  there is: a change that became *available* rather than one that became
  necessary. `next_prime`/`is_prime` are gone, the sign fold is gone — masking
  against a positive mask clears the sign along with the high bits — and the
  probe advance is `(i + 1) & mask`.

  - **Measured, not assumed**, since the whole reason to do it is speed. At
    `-O2`, best of 7, with a control program subtracting the loop and hashing:
    20 000 entries built then looked up eleven times costs 112ms of map work
    before and 87ms after; at 100 000 it is 1088ms against 907ms. So **17–22%**,
    consistently, which is worth having and is nowhere near the 3.7x that
    pre-sizing the same map already buys.
  - **What made it safe is a decision milestone 63 made for another reason.**
    Masking reads only a hash's low bits, so it is the arrangement a hash with
    structure down there ruins — which is exactly why a prime is worth paying
    for when a table cannot know what it will be handed. This table does know:
    `Hash` writes into a `Hasher` instead of returning a number, so every hash
    reaching it has been through `mix`, whose last step is a shift-xor bringing
    the high bits down. A program cannot hand it a raw hash even deliberately.
  - **A bug the switch created, caught before it shipped.** An impl method has
    no visibility control, so `rehash` is public — and it is also the documented
    way to pre-size a map. Under primes any capacity was merely suboptimal;
    under a mask, 5 slots means a mask of 4, every probe visits slots 0 and 4
    alone, and `(i + 1) & 4` never leaves 0. `rehash` now treats its argument as
    a *request*, rounding up to a power of two and raising it to what the live
    entries need. `tests/run/map_capacity.dt` pins it.
  - **`insert`'s probe is bounded now**, like the two searches always were. It
    still cannot run out of slots, so the bound never ends the loop; it is there
    because masking turns a broken invariant into an infinite cycle rather than
    a wrong answer, and a hang tells nobody anything. Removing the rounding
    turns the test from a hang into a named runtime error, which is how that
    bound was checked.

- **Fieldless defs are singletons (milestone 67)** — a `StructDef`/`VariantDef`
  with no fields is constructed once per run and shared thereafter, so
  `Option::None`, `Slot::Empty` and `struct Marker;` stop allocating. Runtime
  only: `heap_struct`/`heap_enum`, a new GC root, no compiler change and no
  opcode change. Design: `runtime.md` "Fieldless defs are singletons".

  The observable behaviour is *nothing*, which is the whole reason it is legal,
  and the two supports are both decisions that were made for other reasons.
  `==` on an aggregate is structural, so two separately-built `None`s already
  compared equal — sharing changes how the answer is reached, not what it is.
  And an aggregate is a handle whose fields can be written through, but
  `OP_FIELD_SET` needs a field index, and a def with no fields has none: there
  is no state to alias. Type erasure adds a third for free — there is one
  `EnumDef` per source enum, so `Option::<Int>::None` and `Option::<String>::None`
  are now literally one object, which they were already indistinguishable from.

  - **Measured, with the control inside the same binary.** Two builds laid out
    differently differ by ~7% on a program that touches none of this, so the
    honest measurement is the *gap* between a construct-only loop and a loop
    identical but for the construction, taken within each build. 5M
    constructions: **97ms of enum work before, 35ms after** — a 64% cut, and the
    residue is dispatch rather than allocation. Holding them is where it shows
    up as more than time: 1 000 000 `Option::None` in an array is 307ms and
    81.1 MiB before, **259ms and 51.8 MiB** after.
  - **`std::map` did not move at all, and that is the finding.** It was the case
    the perf note named — a table is mostly `Slot::Empty` — but `empty_slots`
    builds one `var empty: Slot<K, V> = Slot::Empty;` and pushes *it* n times,
    because a bare `slots.push(Slot::Empty)` does not type-check inside a
    generic function (the unit-variant hint wart — open at the time, closed by
    milestone 69, which deleted the workaround this paragraph describes). The
    workaround
    for a language wart was already the optimisation, by hand. What this
    milestone actually buys is the case nobody hand-optimised: a `None` returned
    from a lookup, a `Tomb` written on a remove, a sentinel built in a loop.
  - **The root has to be strong.** A singleton is reached by the collector from
    its *def*, not from any value, and marking it there rather than clearing it
    in the sweep is not a preference: a weak cache would let a collection that
    lands between two constructions free an object the def still points at, and
    the next construction would hand back freed memory. Immortality is cheap
    here because the set is bounded by the source text — one object per
    fieldless def the program constructs.
  - **The oracle needed sabotaging, and this time it failed the sabotage.**
    `tests/run/unit_variant_shared.dt` originally held 100 000 `Slot::Empty` in
    an array across a churn loop, which looks like the rooting test and is not:
    the array roots the singleton by itself, so the file passed with the root
    walk deliberately deleted. The test only bites once a singleton is built and
    then *dropped* before the collections — then the sabotaged build segfaults.
    (The other two sabotages, sharing a def that does have fields, turn `5 0`
    into `5 5` and `3 1 2` into `2 2 2`.) Milestone 63's rule keeps earning it.

- **Inherent coherence (milestone 68)** — two impls giving one type the same
  *inherent* method name are now an error where the second is written, closing
  the oldest open wart. `impl_defs_share_method` in `sema.c`, called from the
  two places `impl_defs_conflict` already was, plus a pairwise loop for one
  block. No new machinery: the overlap question is `impl_applies` with
  `trait_ref` NULL. Design: `language.md` "Where an `impl` applies",
  `architecture.md` "Coherence".

  - **The granularity is the name, not the impl head** — which is why the rule
    took this long to state. Trait coherence can compare two heads because a
    trait impl *is* one indivisible claim; an inherent impl is a bag of
    independent ones, and std splits `String`'s across three modules. So the
    unit had to be the pair (self type, method name), and the check had to move
    to the *end* of `resolve_impl_decl`: `tc_register_impl` allocates the
    `MethodDef`s zeroed and the names arrive with the items, so there was
    nothing to ask before.
  - **What "inherent" turned out to mean is "not named by a trait".** A name a
    trait declares is reached through a bound or a trait-qualified path, so two
    traits may both declare `go` for one type and an inherent `cmp` may sit
    beside `impl Ord`'s — the `Ord` rewrite gates on the trait before it looks
    for a method, which is what makes that safe. Everything else is reached by
    the receiver alone, and that *includes the extra methods a trait impl is
    allowed to carry beyond its trait*: nothing names those, so they collide
    like any other. The rule fell out of the question rather than being chosen.
  - **Overlap is asked with bounds ignored, from both sides** — the same
    conservative question trait coherence asks, minus the trait. So
    `impl<T: Ord> W<T>` loses to `impl<T> W<T>` and `impl [Int]` loses to
    `impl<T> [T]`: with no specialisation, neither a bound nor a narrower head
    is a way to win a name from a wider impl. Each direction has its own test,
    since the concrete-vs-generic case is caught by only one of them.
  - **THE BUG IT FOUND: milestone 39's own test had been absorbed by std.**
    `tests/run/native_method.dt` declared `@native("string_len") fun len` on
    `String` and `@intrinsic("array_len") fun len` on `[T]` — byte-identical to
    what `std::string` and `std::array` grew later and reach every program
    through the prelude. Every call in it had been running std's body for
    milestones, and it passed because a tautology cannot fail. The names are now
    ones std does not spend (`byte_count`, `sub`, `elem_count`), so the file
    tests its own declarations again. This is the wart's cost made concrete: the
    silent loser was not a hypothetical program's method, it was the test suite's.
  - **Sabotaged four ways**, per milestone 63's rule. Neutering the cross-impl
    check fails 5 tests; deleting the same-block loop fails 1; making
    `method_is_inherent` return true unconditionally over-fires on 9, including
    `std::iter`'s own trait impls; dropping either direction of the overlap test
    fails exactly the test written for that direction.

- **The unit-variant hint gap (milestone 69)** — a fieldless constructor now
  takes its type arguments from a hint that names type *parameters*, so
  `slots.push(Slot::Empty)` checks inside `fun f<K, V>(..)` and `std::map`'s
  annotated-local workaround is gone. One line of `hint_type_args`, plus a
  missing diagnostic the change exposed. Design: `architecture.md` under
  `hint_type_args`.

  - **The gap was a name collision, not a depth limit.** It read as "an
    argument hint solves the variant only as far as `Slot<_, _>` and will not
    then bind those to the enclosing function's `K` and `V`" — a plausible
    story about hints not reaching far enough, and wrong. `TY_GENERIC` interns
    by name, so the `K` a function declares and the `K` an enum declares are
    the *same node*; `Slot<K, V>` therefore interns to `Slot`'s own
    `self_type`, and `hint_type_args` opened with `hint == self_type` as a
    "this hint says nothing" bail. It fired on exactly the case that had the
    answer. Renaming one parameter made the bug vanish, which is what
    identified it: `fun f<K, B>(xs: [Slot<K, B>])` had always worked.
  - **The fix is to stop excluding it, not to add a case.** Seeding from
    `self_type`'s own arguments is the identity substitution, and identity is
    the right answer under both readings of that `T` — the caller's or the
    enum's, they are the same node either way. Deleting the test is the entire
    change to inference.
  - **THE BUG IT FOUND: `infer_unify`'s `TY_GENERIC` branch reported nothing.**
    It returned a bare `false` — the only failure path in that function without
    a `diag_error`, against the convention the rest of the checker follows. It
    had never mattered because a hint's parameters were opened into unknowns
    first, and an unknown absorbs anything; the mismatch surfaced later at a
    `types_equal` with a real message. Seeding puts two distinct generics
    head-on, and a silent `false` becomes `t_poison`, which propagates
    *silently by design*. So `xs.push(Slot::Full(v, k))` against `[Slot<K, V>]`
    went from a reported error to **exit 0 with no output at all**. Caught by
    probing the change for over-fire before trusting it, not by the suite.
    Fixing it made the surviving diagnostic better than the original: two
    errors pointing at the two swapped operands, instead of one comparing whole
    shapes.
  - **Sabotaged both ways**, per milestone 63's rule. Restoring the
    `hint == self_type` bail fails 6 tests — the new one, and every `std::map`
    consumer, since the workaround is no longer there to hide it. Dropping the
    new `diag_error` alone fails exactly one, `tests/fail/unit_variant_hint_swapped.dt`,
    *with exit 0* — which is the regression that test exists to name.

- **An enum's associated functions (milestone 70)** — `Opt::none()` reaches an
  inherent `impl<T> Opt<T>` instead of reporting "unknown variant 'none'". The
  enum path context asks the variant first and falls through to the impl index
  on a miss; an inherent method under a variant's name is refused where the
  impl is written, so the fallback is not a precedence rule. Design:
  `architecture.md` under "A **builtin type** may qualify a path".
  - **The gap was in path resolution, not in the impl.** The declaration
    registered, checked and monomorphised correctly the whole time — nothing
    ever *named* it. That is why the fix is a fallback in one `switch` arm and
    not a change to how enums carry impls, and why the wart entry (which had
    already said so) survived from milestone 69's probing unmodified.
  - **The two resolvers were the same resolver.** `PATHRES_CTX_TYPE`'s body
    became `pathres_assoc_item` and both arms call it, so an enum reaches an
    associated function through the identical code a struct does — the turbofish
    form, the bare generic self that selects by method name, `overload_recv`
    for a trait implemented several times, and a `self` method called as an
    associated function all came along without being written twice.
  - **The miss is the one thing the helper does not report.** Everything else
    (a member off an associated item, type arguments on the function segment)
    is a mistake wherever it appears, but "no such associated item" is only
    half the story for an enum, whose message has to name the variant reading
    too: *unknown variant or associated item 'Nnoe' in enum 'Opt'*. So the
    helper returns a tri-state and lets each caller phrase the miss.
  - **Which reading wins is not a question, because the pair is illegal.**
    Asking the variant first and stopping there would make an associated
    function silently unreachable — precisely the loss milestone 68 was
    written to refuse — so the same answer applies one type over:
    `impl_shadows_enum_variant` reports at the impl. The granularity carries
    over unchanged: a name the impl's *trait* declares is exempt, since it is
    still reached through the receiver and through the trait, and forbidding it
    would turn an enum's variant names into reserved words for every trait it
    implements (`tests/run/enum_variant_trait_method.dt`).
  - **Sabotaged three ways**, per milestone 63's rule. Deleting the fallback
    fails exactly `tests/run/enum_assoc_fn.dt`; neutering
    `impl_shadows_enum_variant` fails exactly
    `tests/fail/enum_assoc_shadows_variant.dt` **at exit 0** — the silent
    acceptance of an uncallable function, which is the regression that test
    names; dropping the `method_is_inherent` filter fails exactly
    `tests/run/enum_variant_trait_method.dt`.
  - **Left open:** an associated function written where a pattern expects a
    variant now draws a second, cascading diagnostic about an unsolved type
    parameter. It is inherited rather than caused — a struct's `P::new` in a
    pattern has always done the same — and is recorded in the warts list.

- **The turbofish for a method call's type arguments (milestone 71)** —
  `it.fold::<Int>(0, f)` replaces the bare `it.fold<Int>(0, f)`, matching the
  `::<..>` an expression path has always required. The lookahead that used to
  tell the two readings apart is deleted, so a `<` after a field or method
  access is unconditionally the less-than operator. Design: `architecture.md`
  under `parse_path(mode)`; the grammar's `postfixOp`.
  - **The ambiguity was never worth a heuristic, because half the language had
    already paid the turbofish.** `PATH_EXPR` required `::<..>` from the start
    and `PATH_TYPE` accepts both — the bare spelling survived only in
    `parse_postfix`, which is the one place a `<` has a live operator reading
    to compete with. Milestone 63 answered that competition by scanning ahead
    for the matching `>` and asking whether a `(` followed; making the two
    spellings agree answers it before either side is parsed and costs no scan.
  - **The change is smaller than the wart it closes.** Sixty lines of token
    scanning deleted, the branch rewritten to call `parse_type_args` (which the
    path already used, so the stack array and its `memcpy` went too), and
    `a.b < c > (d)` — the one shape that satisfied the lookahead's test without
    meaning it — became two comparisons.
  - **Migration cost, measured rather than assumed: one test.** Nothing in
    `std/` spelled a method's type arguments at all, so the suite's only user
    of the bare form was `tests/run/method_type_args.dt`, whose whole subject
    was the ambiguity. It survives as the milestone's own test, rewritten
    around the `::` instead of around the lookahead.
  - **The retired spelling needed a diagnostic, or the parse wart just became a
    message wart.** `h.pick<Int>(7, 9)` now dies as "unknown field 'pick'",
    which describes the lookup and not the mistake. `note_field_is_method`
    scans the impl index and adds a note whenever a failed field name *is* an
    applicable method — legitimate because the language has no method values,
    so the two namespaces never overlap and a field miss is worth re-asking as
    a method. It also catches a method mentioned without calling it, which is
    the older and commoner form of the same confusion.
  - **BUG FOUND WHILE PASSING THROUGH: `parse_type_args` never consumed the
    `>` of an empty list.** `P::<>::new()` checked for `TOKEN_GT`, skipped the
    body, and handed the bracket back to the expression grammar as an operator
    — so the complaint arrived at the `::` two tokens later as "expected
    expression". Latent since the turbofish existed and unreachable from the
    method-call site, which had its own empty-list check; routing that site
    through the shared function is what made it matter.
    `tests/fail/turbofish_empty.dt`.
  - **Sabotaged four ways.** Restoring the lookahead alongside the turbofish
    fails exactly two tests: `tests/fail/method_type_args_bare.dt` **at exit
    0**, and `tests/fail/chained_compare_not_type_args.dt` with the old
    reading's message ("no method named 'n'") in place of the comparison's.
    Accepting a bare `<` with *no* lookahead fails 281 — the pre-milestone-63
    state, and the measure of what that scan was holding up. Dropping the note
    fails exactly `method_type_args_bare.dt`; reverting the empty-list fix
    fails exactly `turbofish_empty.dt`.

- **Pre-sizing a map, in entries (milestone 72)** — `HashMap::with_capacity(n)`,
  `m.reserve(n)` and `m.capacity()`, forwarded by `HashSet`. The top-ranked
  item on the perf list, and the smallest kind of change: one `.dt` file, no
  compiler change *asked for* — though two parser limits had to be lifted before
  it would compile.
  - **The finding is that the units were wrong, not that pre-sizing was
    missing.** `rehash(n)` had been the documented pre-sizing tool since
    milestone 63 and it takes a *slot* count, while every caller has an *entry*
    count. Growth triggers at 3/4 load and a slot count is rounded up to a power
    of two, so `n` entries fit only when `n <= 0.75 * capacity_for(n)` — false
    for every `n` in the top quarter of a power-of-two range. That is a quarter
    of all arguments, and it is the wrong quarter: every power of two is in it,
    and so is every round decimal a person types. `m.rehash(1000)` gives room
    for 768.
  - **Measured, since speed is the whole reason.** At -O2, best of 7, control
    subtracted, 30 000 entries built ten times: **382ms grown, 307ms after
    `rehash(30000)`, 97ms after `with_capacity(30000)`**. The old advice
    recovered 20% of the doubling cost and the new API recovers 75% — for an
    `n` in the good three quarters (20 000, say) the two are identical, which is
    why this was invisible until the conversion was written down.
    `slots_for(n) = capacity_for((n * 4 + 2) / 3)` is the whole of it.
  - **`capacity()` is there to make a rehash observable.** The slot array is
    private, so nothing — no program and no test — could previously tell a
    pre-sized build from a grown one. The alternative was a timing test, and a
    timing test is not a test. It reports occupancy rather than population, so
    tombstones count against it; that is a smaller promise than
    `capacity() - len()`, not a broken one.
  - **`reserve` splits the two counters the way `reserve_one` already did**:
    whether to act reads `used`, since tombstones are what will be in the way,
    while the size to ask for reads `live`, since a rebuild drops them. A map
    whose room is entirely tombstones therefore reserves by *cleaning* and does
    not grow at all — the case that tells the two counters apart, and a line of
    the test.
  - **BUG FOUND: a release-built compiler segfaulted on a module with 1025
    declarations.** `parser_parse` held them in a 1024-entry stack array guarded
    by `assert`, and the release build defines `NDEBUG` — so the guard was
    deleted and the write went past the array (exit 139, confirmed). Unrelated
    to maps; reached because the *other* parser cap, 16 items per impl, blocked
    this milestone outright once `HashMap` wanted a 17th method, and lifting one
    meant looking at the family. Both now grow from the arena, like a block's
    statements. `tests/pass/wide_decls.dt` pins both.
  - **Sabotaged three ways.** Collapsing `slots_for` to `capacity_for` — the
    old, wrong conversion — fails 5 of the 10 lines of
    `tests/run/map_with_capacity.dt`. Restoring either parser cap fails
    `wide_decls.dt`, the impl one also taking `std/collections.dt` itself down
    with it, which is the version that cannot ship silently.
- **Every parser list grows (milestone 73)** — the ~20 fixed buffers milestone
  72 recorded as a wart, lifted together onto one facility. `PLIST` /
  `PLIST_GROW` / `PLIST_PUSH` / `PLIST_TAKE` at the top of `parser.c` back all
  of them, and `parser.c` no longer contains the string "too many". Net −178
  lines, because a cap check and a hand-written right-size copy disappear at
  every site.
  - **The number was never a rule about the language.** The grammar sets no
    limit on arguments, match arms, struct fields or enum variants, and nothing
    downstream does either — so each cap was the size of a buffer showing
    through as a diagnostic, and every one of them refused a program the
    language accepts. 16 match arms is not a lot; 16 struct fields is less.
  - **BUG FOUND, and it is the milestone: a method call's arguments had no
    bounds check at all.** `parse_postfix` wrote `tmp[argc++]` into a bare
    `Expr *tmp[64]` with nothing testing `argc`. `obj.m(a1, ..., a65)` is
    ordinary source and it corrupted the stack — confirmed at exit 134, and
    only because the toolchain defaults to `-fstack-protector-strong`; the
    Makefile asks for no such thing. Every sibling list at least *refused*.
    This is milestone 72's finding one list over, with the guard not deleted by
    `NDEBUG` but never written.
  - **The stack buffer stays, and that is the design choice.** An arena does
    not reuse — `arena_realloc_fn` bump-allocates and abandons — so a list that
    grew from the arena from the start would cost ~2n doubling plus n for the
    right-sized copy, where a stack array costs n. `PLIST` keeps the array as a
    small-size optimisation that *spills* instead of erroring, so the common
    case allocates exactly what the AST node keeps and nothing regresses. It
    also zeroes, inline buffer and spilled extension alike, which two sites
    (a trait's items, an interpolation's segments) had been asking for by hand.
  - **`MAX_ASSOC_BINDINGS` stopped being a limit and stayed as a size.** As a
    limit it answered a question the parser cannot answer: whether a
    `dyn Trait<..>` binding list is total is decided against the *trait's*
    associated types, which `resolve_dyn_type` already does by name — reporting
    the unknown ones and the duplicated ones. A trait with nine associated
    types is not a parse error.
  - **Out of scope, deliberately: sema's `MAX_BOUNDS` (16).** That one bounds a
    real array (`entry->bounds` is allocated at `MAX_BOUNDS`) and carries its
    own diagnostic, so it is a different wart with a different fix.
  - **Sabotaged six ways.** Making `PLIST_GROW` assert instead of grow fails
    **285** tests — growth is not a rare path, `std` itself drives it on every
    build. Reinstating the method-call 64, the trait-item 64, the type-parameter
    8, the array-literal 16 and the `MAX_ASSOC_BINDINGS` 8 each fails **exactly
    one** test, and the right one: the method-call and array caps
    `tests/run/wide_lists.dt`, the other three `tests/pass/wide_syntax.dt`.
  - Also verified under `BUILD=release`, since the last bug in this family was
    invisible in a debug build.

- **A supertrait obligation is discharged (milestone 74)** — `impl DoubleEnded
  for Span` no longer demands an `impl Iterator for Span` beside it. An impl
  implements its trait's whole supertrait closure, taking each of a super's
  items from the block if it writes one and from the super's default body
  otherwise. Design: `language.md` "Supertraits" → "Discharging the
  obligation", `architecture.md` "Supertraits" → "Deriving the super's impl".

  - **The obligation was two claims wearing one name, and only one of them was
    load-bearing.** What makes a `T: DoubleEnded` bound sound is that *some*
    `impl Iterator for Span` exists; that it was written by hand was never part
    of the argument. Milestone 58 conflated them because it had no way to make
    one, so the check became "find one" where it should have been "get one".
  - **Relaxing the check would have been wrong, and one experiment says so.**
    Short-circuiting the obligation loop makes `span.next()` work immediately —
    a receiver call resolves a method by name across impls and never asks which
    trait it came from. The bound and the `dyn` do not: `hello(P)` reports "type
    'P' does not implement trait 'Greet'" and `var d: dyn Greet = P` reports a
    type mismatch, because both search the impl index. So the milestone is a
    *synthesised `ImplDef`*, registered like any other, and the receiver call
    that already worked is the misleading half of the evidence.
  - **Two facts of the existing machinery made it small.** `method_is_inherent`
    has asked `trait_flat_method_index` — the *closure* — since milestone 68, so
    a super's method written in the sub's block was never an inherent name and
    coherence already tolerated it as an extra the trait had not asked for. And
    the derived impl *shares* its `MethodDef`s with the block's, so there is one
    `FunDef` per body: nothing is compiled twice and no name resolves two ways.
    The whole of it is ~140 lines in `sema.c` and no change to codegen, the
    vtable layout, or the bytecode image.
  - **It walks the closure, not the direct supers**, so `trait C: B` and
    `trait B: A` derive both from one `impl C for X` — and in the closure's own
    order, supers first, which is already the order that guarantees a derived
    impl is registered before anything derivable from it is asked about.
  - **It is a fourth sweep of `tc_resolve_module`.** The obligation is
    discharged against what the *module* implements, not against what precedes
    the impl in the file; deriving inside `resolve_impl_decl` would make an
    `impl Loud for P` written above an explicit `impl Greet for P` derive one
    and then collide with it.
  - **THE HAZARD IT CREATES, and the second diagnostic that answers it.** A
    written impl always wins, so derivation can never produce a conflicting
    pair — but that leaves the case where the super is implemented elsewhere
    *and* the block writes one of its items anyway. Before this milestone that
    program was legal and silent; it now runs two different bodies for one name
    depending on how you reach it (`P.name()` finds the block's,
    `Greet::name(P)` / a bound / a `dyn` find the other). Verified by running
    it: "from-loud" then "from-greet" twice. `report_super_item_taken` rejects
    it where the second definition is written — milestone 68's argument one
    relation over. `tests/fail/supertrait_item_taken.dt`.
  - **The obligation diagnostic kept its wording and gained a note**, because
    the error it now reports is narrower than the one it used to: not "you
    forgot an impl" but "nothing supplies `label`". `impl_lacks_super_item` is
    asked by the derivation and by the diagnostic, so the two can never disagree
    about which impls exist.
  - **std was left alone on purpose.** `Rev`, `ArrayIter`, `RangeIter` and
    `CharIter` each carry an `Iterator` impl and a `DoubleEnded` impl whose
    bounds match, and each pair could now be one block. Merging them would trade
    a visible trait boundary for four saved lines in the file a reader goes to
    *for* the trait boundary. `Map` and `Filter` could not merge in any case —
    their `Iterator` impls have the weaker bound.
  - **Sabotaged seven ways, each caught by the test that names it.** Dropping
    the derive sweep fails the three new run tests and `supertrait_item_taken`
    **at exit 0**; requiring every super method rather than only the undefaulted
    ones fails the two run tests that lean on defaults; dropping
    `report_super_item_taken` fails `supertrait_item_taken` at exit 0; walking
    `supers` instead of `flat` fails the transitive case; dropping the note
    fails `supertrait_partial` with the wrong error; not copying the impl's type
    parameters fails the generic case; not copying the assoc types fails
    `supertrait_derived_iter`. Clean under `make sanitize` and `BUILD=release`.
  - **Left open:** the one-block spelling and a separate super impl cannot be
    mixed for the same name, which is the price of "a written impl always wins".

- **A heterogeneous operator (milestone 75)** — `V2 * Float`. The five binary
  `std::ops` traits take an `Rhs` parameter defaulting to `Self`, a receiver
  method call is disambiguated by its arguments, and a bound may name its own
  subject. Design: `language.md` "Generic traits" → "Default type parameters"
  and "`std::ops`", `architecture.md` "Selection by argument type" → "The
  receiver spelling" and "Generic traits" → "Default type parameters".

  - **The wart had the diagnosis right and the conclusion wrong.** It said a
    heterogeneous operator "would need the operator to select an impl by its
    *right* operand, and selection by argument type exists only through the
    written `Meters::from(x)` / `Into::<U>::into(x)` forms" — and it said,
    correctly, that an `Output` alone would not help because it is `Rhs` that
    cannot be *inferred* where the trait is named. All true, and it does not
    follow: **an operator never has to infer the argument, because it has both
    operand types in hand and can write it down.** `ops_trait_ref` builds
    `Mul<Float>` for `v * 2.0`. The three-year-old-looking obstacle was a
    sentence about inference standing in for a fact about knowledge.
  - **THE FINDING: the default is load-bearing, not sugar.** Without it every
    use has to name the argument, and at a bound that argument is the bounded
    parameter — `T: Add<T>`, a bound naming its own subject, which the language
    could not say. `Add<Self>` is not an escape either: `Self` is not in scope in
    a free function. So `Rhs = Self` is what makes the migration *zero* — every
    `impl Add for Int`, every `T: Add`, every `where Self.Item: Add` in `std`
    is byte-identical — and it is also the thing that forced the self-reference
    to be implemented, since a default at a bound expands to exactly it.
  - **A self-referential bound looks like an interning cycle and is not.** A
    `TY_GENERIC` is interned on its *bounds*, so `T` bounded by `Add<T>` appears
    to contain itself. The escape is one line of `subst_apply`: **a `TY_GENERIC`
    is matched by name**, never by pointer. Defining the parameter bound-less
    before resolving its own bounds makes the inner occurrence a *second*
    interned node with the same name, and every instantiation rewrites both, so
    the stratification is invisible everywhere except inside the declaration's
    own body — where three readers collapse it again through one `bounds_rebound`
    helper. This is now a general feature (`tests/run/self_referential_bound.dt`),
    not a private spelling.
  - **`Output` is genuinely blocked, and that is the same finding twice.**
    A generic use would need `T: Add<Output = T>` — an equality binding naming
    its own subject. The binding is *writable* now, but `ty_assoc` discharges it
    by rewriting the projection to the type the binding named, so the result
    would be pinned by a promise the caller makes rather than by what the impl
    bound it to. Adding `Output` would break every generic use of these traits
    before it enabled one, so the result stays `Self` and the dot product
    `V2 * V2 -> Float` stays unspellable. Recorded below as the wart's remainder.
  - **The receiver-side selector is milestone 30's, unchanged; the work was the
    guard.** `impl_index_assoc_select` needed nothing — a method's signature
    carries `self`, so the receiver is matched as an ordinary parameter. What is
    new is `assoc_candidates_differ_in_args`, and its question is *not* "is there
    more than one candidate": it is whether they disagree about the arguments
    they take. Running selection where they agree reports an ambiguity in place
    of an answer — `Into<Fahrenheit>` and `Into<String>` both take `(Celsius)`
    and are settled by the return-type hint, and a milestone 74 derived impl
    shares the written one's `MethodDef` outright. **Comparing the return type
    too puts the first case straight back**, which is why `sigs_take_same_args`
    stops at the parameters; caught by `generic_trait.dt`, which had passed
    through three earlier versions of the guard.
  - **The arguments are resolved once.** `resolve_method_call_typed` pre-resolves
    them hint-free for selection and hands the same types to the argument check
    below, rather than resolving them again — which would re-report a bad
    argument and re-queue every instantiation inside it.
  - **Two messages got better as a side effect.** A mixed pair is now
    `'Cents' does not implement 'Add<Int>'` rather than
    `expected 'Cents' but got 'Int'` on the rewritten call — the wart's own
    complaint that the message "describes the desugaring rather than the
    mistake". And an unsolved *right* operand became its own diagnostic, since
    it is now what chooses.
  - **Sabotaged twelve ways.** Removing the bound-less placeholder fails 5
    (including `ops_operators` and `iter_sum`, which never mention a default);
    neutering `generic_bounds_rebound` fails 4 and `assoc_bounds_rebound` fails
    most of the suite (std's `sum`/`product` drive it); not filling defaults, and
    `ops_trait_ref` ignoring the right operand, each fail most of the suite;
    deleting the receiver-side selection fails 5; the guard always firing fails
    4 (`generic_trait` and both `supertrait_derived`); comparing the return type
    fails 2. The remaining four fail **exactly one test each**: the
    defaulted-must-come-last check, the trait-only check, and the unsolved-rhs
    diagnostic each at exit 0, and `ty_assoc`'s self-collapse with the wrong
    error. Clean under `make sanitize` and `BUILD=release`.
  - **Left open:** no `Output` (above); a default is trait-only, so nothing
    else in the language has one; and `sigs_take_same_args` compares parameter
    types by pointer, which is exact rather than up to the impl's substitution —
    two candidates whose parameters differ only through their own impl
    parameters would be told apart when they should not be. No such pair exists
    in `std` or the suite, and the failure mode is a reported ambiguity rather
    than a wrong choice.

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

## Next (in recommended order)

Estimates are relative to one focused session ≈ the checker-completion
milestone (~900 lines).

Nothing on the main line is *blocked*: every construct the checker accepts in
`tests/pass/in_fixed.dt` now also runs. The list below is what the "known
warts" section would promote first, in the order that pays off soonest — pick
by appetite rather than by necessity. Std module nesting was the one entry whose
cost rose while it waited, and it is milestone 91; nothing here has that
property, so the order is preference again.

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
  milestone 89 there *is* a warning severity for this to use — what is missing
  is now only the analysis, and the policy call about whether shadowing deserves
  one at all (Rust does not warn on it)
- a refutable `var` binding whose column type inference never pinned down is
  accepted (the tri-state answer reports nothing) and traps at runtime via
  `OP_MATCH_FAIL` instead of at compile time
- there is no way to *ask* about a warning: no `#[allow]`, no `-W`, no
  `-Werror`. Suppression is the one built-in policy (std, unless it is the
  root), so a warning a program has decided to live with cannot be silenced and
  one it wants enforced cannot be made fatal
- a directory named `std` holding a file named after a std module is compiled
  *as* that module when named on the command line (milestone 90), so a user who
  happens to have `std/cmp.dt` gets it checked with no prelude and `@lang`
  honoured, which usually fails confusingly. Deliberate: `use std::cmp` is
  intercepted before the filesystem, so such a directory was never reachable as
  `std::` anyway — but the diagnostic does not explain what happened. Milestone
  91 widens the reach without widening the explanation: the name may now span
  directories, so `std/collections/hashmap.dt` is adopted too
- a group's facade is hand-written and unchecked (milestone 91's remainder).
  `std::collections` is a module only because `std/collections.dt` sits beside
  the directory and `pub use`s what is under it; nothing generates that file or
  notices when it falls behind, so a module added to a group is reachable by its
  full path and silently missing from the short one
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
  them, and a qualifier is not re-exportable (it is not an item)
- `pub` is ignored on the `impl` keyword and rejected on a method or a struct
  field (see above); there is no sub-module visibility, so `pub` means something
  only on a top-level item, at module granularity
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
