# ducktape — roadmap

## Start here

> **This branch is a fork.** `feature/m122-ir` leaves the main line at
> milestone 120, before either of the two rival milestone 121s (`main`'s "a
> file is never held open" and `feature/m120-file-handle`'s "a file is held
> open"). It is a compiler branch and touches nothing either of them does, so
> it is a merge and not a third alternative — but the 121 question has to be
> settled on its own terms first.

**Last landed:** milestone 122 (`75522a3`) — **codegen emits an
instruction list, not bytes**. `IrBody` (`include/ir.h`, `src/ir.c`) holds one
entry per instruction; `ir_emit` walks it into the `Chunk` at the end of each
body. A forward jump names a label instead of an offset patched back into the
code, and the peephole's three watermarks are indices rather than byte
positions. Behaviour-neutral by construction: 714 tests, clean under
`make sanitize`, and every one of the 203 emittable test programs produces a
**byte-identical** image against 120.

**Landed before it:** milestone 120 (`3543157`) — **the name codegen no longer
resolves**. A reference is matched on the `VarEntry` the checker resolved it
to, not on its name, so `CgDeferSkip` and `Cg.locals_base` are both gone.

**What 122 is for.** It is step one of promoting `src/cfg.c` from a lint's
private graph to the backend's IR — the wart "the graph has exactly one
consumer". Nothing is faster yet and nothing was meant to be. The next step is
the one that pays: **lower `defer` into explicit exit edges at IR
construction**, which would close "a body holding a `defer` is refused a
splice" and m116's "thread declines where a `defer` sits between a construction
and the `return`" by deleting what they are about, rather than fixing it. After
that, inlining as a block splice with renamed operands, which is what makes
"an inlined body is still compiled on its own" reachable.

**Nothing is blocked, and nothing is owed.** Milestone 95 closed the last entry
that was not a matter of appetite, and 103 the last one that was already
*designed*. Every item under "Next" is a choice.

**Item 1 of "Next" — the iterator, and the compiler work behind it — is
spent**, at milestone 118. Ten milestones took `it.next()` from +77.5ns/elem
over the plain `for` to +7.7, and the direction has no step left in it; its
entry keeps the reasoning because each finding outlived its milestone.
**Everything under "Next" is breadth** — 119 left one compiler item that was
not, and 120 spent it — so the three below are all appetite.

2. **std breadth on the natives** — every piece with a design question in it is
   spent, so what remains is typing.
3. **`Eq`** — recorded as *declined*, by two consumers that were asked for it by
   name. Read the entry before reopening it.
4. **`std::io` past one read** — one decision in it: whether a file *handle* is
   worth a type, which is really the question of who closes it.

**How to read the rest of this file.** `Done` is one entry per milestone, oldest
first, each pointing at the commit that holds the full write-up — `git log` is
the home for a milestone's story, not this file. `Known warts` is the checklist
to pick from when nothing under `Next` appeals; entries there name the milestone
that recorded them. Milestones through 119 live in `history/`.

## Done

Milestones **through 54** are in `history/done-through-m54.md`, **55-75** in
`history/done-m55-m75.md`, **76-99** in `history/done-m76-m99.md` and
**100-119** in `history/done-m100-m119.md`, split out to keep this file small.
Everything from 120 on is below.

- **120. The name codegen no longer resolves** (`3543157`) — codegen matches a
  reference against the `VarEntry` the checker resolved it to, not against its
  name, which is the identity `src/cfg.c` already reads. `CgDeferSkip` and
  `Cg.locals_base` are deleted with the walk they corrected: a shadowed name is
  a different binding, and a spliced body's bindings are its own. The plumbing
  was `ParamDef.entry`, the one binding form that had none, and `self` became
  the ordinary parameter binding the checker always made it.
  Design: `runtime.md` "Codegen shapes" (the second bullet) and "What a spliced
  body may see", `architecture.md` "Scopes and name resolution".
  **THE FINDING: the reverse scan is load-bearing, and shadowing is not why.**
  Sabotage turned it around and three tests broke, where "one binding is one
  entry" says it cannot matter — but a body spliced into *itself* puts one
  binding on the stack twice, since `cg->inlining` blocks the second round of
  self-recursion and not the first. The innermost is the callee's own, so
  `cg->locals` is a scope stack after all. Chasing it found a crash older than
  the milestone: `compile_closure` zeroed the splice chain, so `|| f(..)` inside
  `f` reset both the depth cap and the recursion check and expanded until the C
  stack ran out; the child compiler now inherits both. Sabotage 9/10, the
  survivor unreachable rather than untested (a closure's `ParamDef` mirror — a
  splice needs a call by name). Remainder: none.

- **122. Codegen emits an instruction list, not bytes** (`75522a3`) —
  `IrBody` holds one entry per instruction and `ir_emit` walks it into the
  `Chunk` when the body is done. A forward jump names a label that `patch_jump`
  binds; `last_get_local`, `last_target` and `last_exit` become indices, so a
  rewrite reads `ir_last` instead of decoding operands back out of
  `chunk->code`. The constant pool stays on the `Chunk` — it is indexed as it
  is filled and never needed deferring.
  Design: `runtime.md` "The instruction list", and the two paragraphs of "The
  peephole" that described byte positions.
  **THE FINDING: the peephole's watermarks are not a workaround for bytes.**
  The obvious follow-up is to delete `last_get_local` and `last_enum` now that
  `ir_last` can be asked what the last instruction is, and both would be wrong:
  compound assignment emits its own `OP_GET_LOCAL` and `as?` its own `OP_ENUM`,
  and neither is eligible for the rewrite its watermark guards. The positions
  mark a *site*, not an opcode; all the list retires is the sentence saying why
  they had to be remembered.
  No test file — there is no behaviour to pin. The check is byte-identity: 203
  programs `--emit-bc`'d on both binaries and compared, which is stricter than
  a green suite and is what the next two steps can be held to.
  Remainder: it is not a value IR — an instruction is still one the VM
  executes, and nothing reads the list between construction and emission.

## Next (in recommended order)

Estimates are relative to one focused session ≈ the checker-completion
milestone (~900 lines).

Nothing on the main line is *blocked*: every construct the checker accepts in
`tests/pass/in_fixed.dt` now also runs. The list below is what the "known
warts" section would promote first, in the order that pays off soonest — pick
by appetite rather than by necessity. Nothing here is load-bearing any more:
the module system was the one item that replaced infrastructure known to be
wrong, and it landed in milestones 94–95.

1. ~~**The allocation the iterator pays, and the compiler work behind it.**~~
   **Spent at milestone 118**, ten milestones after it opened: `it.next()` cost
   77.5ns/elem over the desugared loop and now costs 7.7. Everything below is
   the reasoning that got there, kept because each step's finding outlived it —
   there is no step left to take.

   **Where it stands after 117: ~15ns/elem, from 77.5.** The allocation, the
   frame, the call's dispatches and the `Option` itself are all gone; what is
   left is the iterator struct's own field reads and writes, which 117 can
   delete for a local but not for a *receiver* — the one thing still open under
   this heading (step 3). The rest of this item is the reasoning that got there,
   kept because each step's finding outlived it.

   **Step 1 is done, in two milestones.** 109 found the `Some(x)` was *two*
   allocations and made it one; 110 made it none, by carrying a one-field variant
   in the `Value`. Together they took `it.next()` from +77.5 to +51ns/elem and
   every struct literal ~40%. **What that changes for steps 2 and 3: the next
   term is no longer an allocation.** The `Option` is ~75% of the overhead rather
   than 85%, and the rest is the call frame plus the iterator struct's own field
   traffic — which is what inlining is for, so the ordering below still holds
   while the shortcut at the end of this item has lost most of its prize.

   **THE FINDING, before any of it is built: `next()` is expensive and the call
   is not why.** The frame is 6% and the `Option` is 78%, so perfect inlining
   takes `next()` from 103ns to ~98 and changes no decision — milestone 108's
   chunk-granularity rule for streaming stands either way. Milestone 113 built it
   and the measurement agreed: 3.7ns of 45.6.

   That reframes what is worth building, into three steps that are each useful
   alone and are listed in the order their payoff arrives:

   1. ~~**`Option<T>` that does not allocate.**~~ **Done — milestone 110**, and
      it generalised on the way: it is any one-field variant of any enum, not
      `Option`, because a variant pointer identifies one and a lang item would
      have had to be taught to the image format. `runtime.md` "A one-field variant
      needs no object". What is *not* done is a `T` too wide for a word — a
      `range` field, or a nested `Some` — which still allocates.
   2. ~~**Inlining.**~~ **Done — milestone 113**, and every piece it was
      predicted to need was already there: the context turned out to be the
      `Instance` itself, milestone 88's stack-position slots meant no prologue at
      all, and milestone 98's landing pad *is* an inlined `return`.
      `runtime.md` "Inlining". It bought 8% on `it.next()` and, more usefully,
      the number underneath: **a frame is cheap and a call's cost is opcodes**.
      Milestone 114 then took the three dispatches it left (`runtime.md` "The
      peephole"), which is where this stops: a call by name costs nothing, and
      nothing further about *calling* is on the table.
   3. **A CFG, and then what a forward analysis over it buys.** **The graph is
      done — milestone 115**, built per body in `src/cfg.c` and spent on the
      second of its two named consumers, milestone 107's per-store liveness.
      `architecture.md` "Liveness, and the store grain". What it carries is
      control flow and *binding* events, which is all liveness needed.

      Of the two consumers this step named for the *forward* direction, one is
      **done and did not use the graph**: milestone 116 deleted the variant a
      match never needed built, and the finding is that the fact is not a
      dataflow at all — a path can be asked what it pushed while it is still the
      last thing emitted, and the paths that cannot answer are left the tag test
      they had. `runtime.md` "Threading a match into its constructions".

      The other, **escape analysis, is done — milestone 117, and it wanted no
      graph either.** This file predicted it would need def-use over *values*,
      because an aggregate is a handle and two names reach one object. Both
      halves are true and the conclusion did not follow: with no borrows to take
      and no address to pass, a *bare mention* is the only way a handle travels,
      so "does every mention read a field" is a question about syntax and the
      graph has nothing to add to it. `runtime.md` "Escape analysis and scalar
      replacement".

      So the CFG has one consumer, liveness, and the forward direction found
      nothing to ask it. The prerequisite that turned out to matter for the
      graph was neither of the two it was built for: it was that the checker and
      codegen disagreed about which binding a shadowed name meant.

      **What is left of this step** is scalar replacement across a *splice
      boundary*: an iterator is its `next()`'s receiver, and passing a receiver
      is the value leaving whole, so `var it = xs.iter()` declines. Making it
      not decline means binding a spliced `self` onto the caller's field slots,
      which needs the inline decision known at the declaration and a
      materialising fallback for where codegen later declines it. That is ~10ns
      of `next()`'s remaining 15.

   **The peephole: both halves now spent.** The *splice-boundary* half — a read
   of a parameter already in place, the slide that removed the value it just
   copied, and the jump to the instruction after next — is **done, milestone
   114**, and it took a call by name to zero.

   **The other half was never a peephole *and never needed a graph either*.**
   This file argued at length that `while var Option::Some(x) = it.next()` could
   not be folded by looking at the last instruction, because a spliced `next()`
   reaches its landing pad from two returns and milestone 114's `last_target`
   declines a rewrite at a merge. Both halves of that are true and the
   conclusion did not follow: the fold does not belong at the merge, it belongs
   at **each return**, where the construction *is* the last instruction and
   `last_target` says so. Milestone 116 is that, plus a merge that keeps the old
   tag test for whatever arrives without an answer.

   **On throwing the standard library away.** It is ~20 modules written *in
   ducktape*, so nuking it is cheap and always will be — which is itself an
   argument for spending sessions on the compiler rather than on breadth, since
   every std module written now is one to be discarded and every compiler
   capability survives. What is *not* cheap is the surface the compiler knows by
   name, and that is the short list to stay conservative about: **14 `@lang`
   items** (`display`, `ord`, `option`, `iterator`, the six operators, three
   `pad_*`, `float`), the prelude closure in `src/module.c`, and the seven
   builtin type names in `type_named_builtin`. `Option` being one of those is
   why step 1 is a compiler milestone and not a std one.

2. **Growing std on top of the natives** — the mechanism landed in milestone
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

   The one follow-up measured after milestone 108 — that a `Some(x)` allocates —
   moved out of this item and became item 1 above: it is a change to the *value
   representation*, so it is not breadth on the natives at all.

   Every piece of this item that had a *design question* behind it is spent.
   Settled here, each written up in its own commit and Done entry: an iterator
   source (m60), a string's characters as a walk (m61), a sorted `[T]` (m62), a
   map and set (m63), the first combinators and their move onto `Iterator`
   (m47-48), `fold`/`enumerate`/`zip`/`take` (m49), `flat_map`/`skip` (m50-51),
   the associated-type bounds behind `chain` and `sum` (m52-54, 76), `rev` and
   the supertrait it needed (m58), the breadth after it (m59), `sum`/`product`
   over an `Add` trait (m55), `std::assert` (m57), padding (m34) and the
   `{v:>8}` spec over it (m35), text operations (m32), selection by argument
   type (m30) and its receiver spelling (m31), ordering over `Ord` (m38), and
   the heterogeneous operator that was once the largest open design question
   here (m75). What is left under this item is breadth with nothing open in it:
   an ordered map over `lower_bound`, more `Hash`/`Ord`/`Display` arities.

3. **A custom equality trait (`Eq`) — the named consumer has now declined it.**
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

4. **Growing `std::io` past one read.** Milestone 108 spent the item that stood
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

Grouped by where they live. Each entry names the milestone that recorded it;
nothing here is scheduled — this is the checklist to pick from when nothing
under "Next" appeals.

### Diagnostics: one mistake, more than one message

Each of these is a *second* diagnostic for what a reader sees as one mistake, or a message that describes the rewrite rather than the error.

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
- a unit struct's name cannot be bound as a variable any more: `var Marker =
  7;` is a struct pattern against an `int`, and the diagnostic ("expected
  struct type in struct pattern") describes the rewrite rather than the
  mistake. Rust behaves the same way; the message could be kinder
- the unused-binding warnings of one function come out in **scope-close** order
  rather than source order, so an inner block's precede an outer one's whatever
  their lines. Sorting the bag would need notes to stop attaching to the
  diagnostic before them by position (milestone 89). Milestone 115's dead-store
  reports sort themselves before they are emitted, which is the same fix done
  one pass early rather than the general one

### Lints and `@allow`

The lint family's remainders. What is missing is mostly grain — which thing a warning is about, and how narrowly it can be silenced.

- no shadowing diagnostics for `var` (`vscope_define` todo); top-level item
  names do collide, but a `var` may silently shadow one in the same scope. As of
  milestone 89 there *is* a warning severity for this to use, and as of 92 the
  entry to hang it on (`VarEntry.span`, `used`) — what is missing is now only
  the analysis, and the policy call about whether shadowing deserves one at all
  (Rust does not warn on it). Milestone 115 fixed the half that was not a
  policy call: the checker and codegen used to disagree about *which* binding a
  shadowed name meant, so the shadowed one now merely warns as unused
- **a parameter is reported twice over, in two wordings** (milestone 115).
  A parameter the body overwrites and never reads gets "value *passed* to" from
  the graph, which knows the store is the signature's; one that is never read at
  all gets "value *assigned* to" from `vscope_pop`, which cannot tell a
  parameter from a local. Both are `unused_assignment`, and the second is the
  wrong word for the same reason the first was
- **the graph has exactly one consumer** (milestone 115). Liveness is it, so
  `cfg_check_body` builds the graph, answers and drops it. Both consumers the
  *forward* direction named turned out not to want it — 116 read a construction
  off the last instruction, 117 asked a question about syntax — so nothing is
  waiting on the def-use chains the events do not carry, and the second question
  that would justify keeping a graph per body has yet to turn up
- a lint's level is **all-or-nothing over a compile**: `-W` (milestone 97) takes
  no module path, so one module cannot be held to a level the rest is not, and
  the only per-declaration control is `@allow`, which only goes downwards. Nor
  is there a level that *beats* an `@allow` — Rust's `forbid` — so a build that
  must not be silenced anywhere has to grep for the attribute
- an allow's grain is a whole **declaration**, since an attribute is what
  carries one. There is no statement or expression form, and none on a trait
  item — a default body is covered by its trait, one scope wider than it
  should be. Rust's `#[allow]` on a statement is the shape this is missing
- a **method is not an item**, so `unused_item` (milestones 111-112) cannot
  report one, and cannot until method visibility exists: a method is as visible
  as its impl, so the only thing that could be said about who reaches it is what
  the impl's self type already answers
- `PAT_WILDCARD` is **unreachable**, and milestone 92 depends on it being so.
  `scan_identifier` runs before the `switch` that would make a lone `_` a
  `TOKEN_UNDER`, so `_` is an ordinary identifier and every `_` in a pattern is
  a `PAT_BIND` named `_` — which is exactly why the `_` opt-out needed no
  spelling of its own. The cost is a pattern kind implemented in five places
  (sema ×3, codegen ×2, ast) that nothing constructs, and `var y = _;` being a
  legal read of a binding nobody meant to make

### Types, traits and coherence

The largest group, and the one where a wart is usually a decidability question rather than an oversight.

- sema's `MAX_BOUNDS` (16) is the last fixed cap of the family milestone 73
  cleared out of the parser. It bounds a real array and has its own diagnostic,
  so it is a limit rather than a hazard
- a *written* impl always wins, which is what keeps milestone 74's supertrait
  derivation from ever conflicting — so an item of a super that something else
  already implements may not also be written in the sub's block. The one-block
  spelling and a separate super impl cannot be mixed for the same name
- a refutable `var` binding whose column type inference never pinned down is
  accepted (the tri-state answer reports nothing) and traps at runtime via
  `OP_MATCH_FAIL` instead of at compile time
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

### Modules, visibility and imports

- the **embedded std bypasses the UTF-8 intake check**. `mod_parse` takes
  `std_module_source()` when `is_std && !from_disk`, so a library module's bytes
  never pass `read_file` — the same file is validated under `--std-module`
  (which sets `from_disk`) and trusted on every normal compile. Not reachable
  today: std is ASCII and `make test` lints every module through the disk path,
  so the suite would catch it. But milestone 101's invariant rests on two
  checked intakes plus one trusted one, which is not what it claims
- **an import's impl set arrives transitively, so a deliberate `use` can read
  as unused.** `std::io` imports `std::bytes`, so `use std::io::print;` hands a
  program the whole `Bytes` surface — and `unused_import` then reports an
  explicit `use std::bytes;` beside it, correctly under its own rule (removing
  that line loses no impl that was selected) and unhelpfully as advice. The
  prelude has always done this for `StringBuf` through `std::string`; milestone
  108 is the first time a module a program imports *by choice* became a source
  of it. What would fix it is a way to say an import is wanted for its impls,
  which is the same missing spelling `language.md`'s table already records
- a module may be named after a builtin type (`mod char;`, which std does), but
  a builtin name resolves before any module, so `char::` reaches the *type*'s
  items. That is why std's spelling works — `std::char` is all `impl char` — and
  why a free function in such a module has no bare path at all. Milestone 99
  chose a note over a refusal here; the shadowing itself stands, and milestone
  103 made `std::string` the second instance and the first one that bites:
  `join`/`concat`/`from_chars` now need `use std::string::join;` or
  `use std::string as strings;`. A qualifier must be a bound single segment, so
  there is no `std::string::concat(..)` spelling to fall back on either
- `pub` is parsed and ignored on the `impl` keyword itself, and *rejected* on a
  method: `pub fun` inside an `impl` block is "expected impl item". **Method**
  visibility does not exist — a method is as visible as its impl — which is why
  the operations std wants to keep to itself (`std::array`'s raw `pop_last`,
  `std::iter`'s `char_at` / `prev_boundary`, `std::string`'s `char_width`) are
  private *free functions* rather than methods. **Field** visibility does exist
  and this entry used to deny it: a field takes `pub` and is private to its
  module without one. Visibility therefore exists at three grains that do not compose: an item
  takes `pub`, a field takes `pub`, a `mod` takes `pub` — and a method takes
  nothing, so it is as visible as its impl however private the path to it
- a **module qualifier is not re-exportable**: `pub use a;` binds `a` for the
  importer and hands nothing on, because a qualifier is not an item. Milestone
  100 globbed the items but left this — a facade can re-export every type below
  it and still cannot offer the *module* the types came from
- module dedup is lexical, so one file reached by two different spellings
  (a symlink, say) would load twice and collide

### The standard library

Mostly the cost of shipping an impl: a std impl for a common type takes that pair away from every program that imports the module.

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
- `std::array` is no longer a leaf: `pop` returns an `Option`, so importing any
  of it reaches `std::option` and, transitively, every impl `std::fmt` and
  `std::cmp` ship. A program that wanted `push` and its own `impl Display for
  int` cannot have both. Milestones 25 and 26 lengthened that chain — `std::cmp`
  now reaches `std::string` and `std::char`, and `std::char` reaches
  `std::panic` — but not its cost, since none of the three ships an impl to
  inherit
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

### Text and Unicode

The corner where the honest answer is a data table nobody has shipped.

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
  and is roadmap item 4 below. The check itself is not the cost the byte-indexed
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

### The runtime, codegen and images

- a definition nothing reaches is never compiled, so a construct the VM
  refuses inside one goes unreported — the diagnostic arrives only if
  something calls it (`tests/run/unreachable_body.dt`). The checker is
  unaffected; this is codegen only
- an `@intrinsic` cannot be used as a value (it is an opcode, so there is no
  body for a global slot to address) — reported at codegen, unlike the
  `@native` beside it which is fully first-class
- **memory exhaustion is not recoverable.** Since milestone 106 it is at least
  *defined* — `heap_alloc` prints `out of memory` and exits — but a program
  cannot observe it, because there is no unwinding to observe it with. It is
  the same shape as a panic, minus the frames
- `for x in xs` re-reads the length each iteration, so pushing to the array
  being iterated extends the loop instead of iterating a snapshot. There is no
  borrow checker to forbid it, and nothing diagnoses it
- the slot spaces are two bytes wide, so 65536 functions/structs/enums/vtables
  is a hard program-wide ceiling (reported, not silently truncated) — and every
  instantiation of a generic and every (trait, type) pair coerced spends one,
  so it is reached by *use* rather than by how much is written
- an unsupported construct inside a trait method is reported at the coercion
  site that builds the vtable, since that is where the body is first demanded
  — the same "only seen where it is instantiated" limitation generics have
- a bytecode image is structurally validated (bounds, indices, counts) but the
  code itself is not verified: an image with a plausible header and nonsense
  instructions can still crash the VM
- an image carries no source, so a runtime error in one reports function names
  with no line information
- **an inlined call is missing from a runtime error's trace** (milestone 113).
  The trace is a walk over live frames and a spliced body has none, so the
  function whose code raised the error is not named. Nothing else can see the
  splice, which makes the trace the one place inlining is observable
- **an inlined body is still compiled on its own** (milestone 113): the target
  is reached, slotted and queued before the decision to splice it, so a
  definition inlined at every one of its call sites is compiled twice and the
  standalone copy is dead code in the image. Dropping it needs a pass after the
  queue drains, and the slots are already baked into the emitted code
- **an argument that is already a local is still copied** (milestone 114). A
  parameter is a fresh stack position, so `it.next()` pushes a second copy of
  `it` and the epilogue slides it away — two dispatches per call that a
  parameter *aliasing* the caller's slot would not spend. It is not a peephole:
  the alias is only sound if no argument can be evaluated after it and the body
  neither assigns the parameter nor captures it, which is three questions about
  the whole call rather than one about the last instruction
- **only a one-field variant is unbuilt** (milestone 116). Threading deletes the
  tag test and the field read for any arity, but the construction itself only
  where the field can stand in the subject's slot — a two-field variant is still
  an `ObjEnum` and still allocates, so its arms gain ~16% where a one-field
  variant's gain ~73%. Wider arities would need the slot to hold several values,
  which is a slide that keeps more than one
- **a threaded subject must be a call node** (milestone 116). The claim is armed
  by the site compiling the subject's own call, so a subject that is a block, an
  `if`, or anything else with a construction at its tail arms nothing and keeps
  its tag test. The fall-through claim covers the one other shape that matters,
  a subject that *is* a construction
- **a threaded `match`'s trap is unreachable** (milestone 116). Every tag no arm
  names goes to `CG_THREAD_OTHER`, which for a `match` is `OP_MATCH_FAIL` — but
  an arm that could leave a tag uncovered (a guard, a binding arm) declines
  threading, and exhaustiveness covers the rest. It is a net with no test behind
  it, like `compile_coerce_dyn`'s abstract-target guard
- **a body holding a `defer` is refused a splice** (milestone 119).
  `Cg.defers_base` already makes one come out right — a spliced `return` runs
  the callee's scopes and stops at the caller's — and the sabotage that turned
  the guard off survived the whole suite. It is conservatism, not a
  requirement; relaxing it is an optimisation and wants a bench row
- **m116's thread declines where a `defer` sits between a construction and the
  `return` carrying it** (milestone 119), since the claim is read off the last
  instruction emitted and a deferred body is instructions. Correct, and visible
  only as speed — an iterator's `next` that defers keeps its tag test
- **the read/slide fold only fires for the first parameter** (milestone 114).
  The slide leaves its value at the lowest slot it removes, so only a read of
  *that* slot is already in place; `return b` from a two-parameter body still
  copies and slides. Reaching the others needs a store, which is the same
  dispatch it would save

### Absent on purpose, or not yet wanted

Listed so the absence reads as a decision rather than an omission.

- no unsigned integer type, so `>>>` stands in for one and a bit pattern with
  the top bit set prints negative
- `while true { }` still types `()`, and deliberately: its condition is an
  expression, and the checker reads types rather than values. `loop { }` is the
  spelling that asks no condition (milestone 86)
- `!` is refused in the positions that ask a **structural** question rather
  than a flow one: `if panic("x") { }` wants a `bool`, `for x in panic("x")`
  wants an `Iterator`, and `r?` inside a `-> !` function wants the same enum
  back. Each reports a plain mismatch. Rust accepts all three; nothing is lost
  by refusing them, since the code below is unreachable either way
- a panic does not unwind — no `catch`, and no way for a program to observe one
  and keep running. `!` is only a *type*; the runtime behaviour behind it is
  "print the frames and stop". A `defer` does not run on that path either
  (milestone 119), which is what keeps `defer` ergonomics rather than a
  guarantee, and why item 4's "who closes a file handle" still wants a library
  answer

### The test suite

- **the failure path of `io_read_file` is only half testable.** A read that
  fails partway restores the caller's buffer, and the suite cannot build one:
  the reachable failures (a missing file, a directory) append nothing before
  they fail, so the restore is a no-op wherever it can be observed. The
  `ferror` branch itself is tested — a directory opens and then refuses
- `make sanitize` is a separate run, not part of `make test`: a sanitizer
  finding is not a test failure, so the suite stays green while the binary is
  doing something undefined. Nothing enforces that it is run before a commit
