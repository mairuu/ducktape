# ducktape — completed milestones 55 through 75

Split out of `references/roadmap.md` so the live roadmap stays greppable, the
same way `done-through-m54.md` was. This is history: nothing here is a plan.
Milestones 76-99 are in `done-m76-m99.md`; the roadmap keeps 100 onward, the
"Next" list and the open warts.

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
    only structural difference from `Ord`'s.** `a.cmp(b)` is an int the outer
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
    named — so a heterogeneous `V2 * float` would need the operator to select an
    impl by its right operand, which exists only through the written
    `Meters::from(x)` / `into::<U>::into(x)` spellings. An operator has neither,
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

  The impls `std::ops` ships for `int`/`float`/`String` are *not* what makes
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
  the documented one — a program can no longer write its own `impl Add for int`.

  Two existing tests changed their premise rather than their expectation, which
  is the clearest measure of what moved: `tests/fail/generic_add.dt` used to
  assert "there is no operator overloading" and now asks for the bound, and
  `tests/fail/unary_not_numeric.dt` used to say `-` requires a number and now
  reports a missing `impl Neg`.

- **A trait object satisfies its own bound (milestone 56)** — `first(d)` where
  `d: dyn Iterator<Item = int>` and `first` takes an `I: Iterator` compiles.
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
    that one function, this is what collapses `I.Item` to `int` everywhere at
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
    message. A ducktape function sees a `bool`. So `assert(cond)`'s message is
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
    the closure, and a `dyn DoubleEnded<Item = int>` is an `Iterator` in every
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

  - **`Range` becomes a nameable type**, joining `String`/`StringBuf`/`char` as
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
    of one sequence**, so it answers with the first int *past* the end and the
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
  `CharIter` rather than a `[char]`. Design: `language.md` `std::iter` →
  "Sources", and `std::char` → the String/char split.

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
    A `char` *is* a scalar value and a scalar value determines its UTF-8 width,
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
    `fun hash(self) -> int`: an impl says only *which parts of me matter*, the
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
  - **No `impl Hash for float`**, and the contrast with `impl Ord for float` is
    the point: an order is total and so had to put NaN *somewhere*, while a
    table may decline a key. `NaN != NaN` would make such a key unfindable in a
    table that compares with `==`, and a truncating `as int` is the only number
    a float can reach here anyway.
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

- **Bitwise operators (milestone 65)** — `& | ^ << >> >>>` and unary `~`, int
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
    (`HashMap<String, Option<int>>`), and `parse_type` consumes those one at a
    time, so a scanner that fused `>>` would break every type that nests. The
    scanner therefore emits single angle brackets and `parse_shift` fuses
    adjacent ones, which also leaves milestone 63's `looks_like_type_args` scan
    exact. The adjacency test is what separates `a >> b` from `a > > b`, and it
    is the only place in the grammar where whitespace changes a parse.
  - **Precedence is Rust's, not C's**: every bitwise operator binds tighter than
    comparison, so `a & b == c` is `(a & b) == c` rather than C's fifty-year-old
    `a & (b == c)` trap. Shifts stay looser than `+`, which is the half of C's
    ordering worth keeping.
  - **`>>` propagates the sign and `>>>` shifts in zeros**, because `int` is
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
    int, float, String or a call to `Add` — while a bitwise operator has exactly
    one operand type. So they unify with `int` rather than testing for it, and
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
  `EnumDef` per source enum, so `Option::<int>::None` and `Option::<String>::None`
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
    `impl<T: Ord> W<T>` loses to `impl<T> W<T>` and `impl [int]` loses to
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
  `it.fold::<int>(0, f)` replaces the bare `it.fold<int>(0, f)`, matching the
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
    message wart.** `h.pick<int>(7, 9)` now dies as "unknown field 'pick'",
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

- **A heterogeneous operator (milestone 75)** — `V2 * float`. The five binary
  `std::ops` traits take an `Rhs` parameter defaulting to `Self`, a receiver
  method call is disambiguated by its arguments, and a bound may name its own
  subject. Design: `language.md` "Generic traits" → "Default type parameters"
  and "`std::ops`", `architecture.md` "Selection by argument type" → "The
  receiver spelling" and "Generic traits" → "Default type parameters".

  - **The wart had the diagnosis right and the conclusion wrong.** It said a
    heterogeneous operator "would need the operator to select an impl by its
    *right* operand, and selection by argument type exists only through the
    written `Meters::from(x)` / `into::<U>::into(x)` forms" — and it said,
    correctly, that an `Output` alone would not help because it is `Rhs` that
    cannot be *inferred* where the trait is named. All true, and it does not
    follow: **an operator never has to infer the argument, because it has both
    operand types in hand and can write it down.** `ops_trait_ref` builds
    `Mul<float>` for `v * 2.0`. The three-year-old-looking obstacle was a
    sentence about inference standing in for a fact about knowledge.
  - **THE FINDING: the default is load-bearing, not sugar.** Without it every
    use has to name the argument, and at a bound that argument is the bounded
    parameter — `T: Add<T>`, a bound naming its own subject, which the language
    could not say. `Add<Self>` is not an escape either: `Self` is not in scope in
    a free function. So `Rhs = Self` is what makes the migration *zero* — every
    `impl Add for int`, every `T: Add`, every `where Self.Item: Add` in `std`
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
    `V2 * V2 -> float` stays unspellable. Recorded below as the wart's remainder.
  - **The receiver-side selector is milestone 30's, unchanged; the work was the
    guard.** `impl_index_assoc_select` needed nothing — a method's signature
    carries `self`, so the receiver is matched as an ordinary parameter. What is
    new is `assoc_candidates_differ_in_args`, and its question is *not* "is there
    more than one candidate": it is whether they disagree about the arguments
    they take. Running selection where they agree reports an ambiguity in place
    of an answer — `into<Fahrenheit>` and `into<String>` both take `(Celsius)`
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
    `'Cents' does not implement 'Add<int>'` rather than
    `expected 'Cents' but got 'int'` on the rewritten call — the wart's own
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

