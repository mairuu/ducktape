# ducktape — roadmap

## Start here

**Last landed:** milestone 118 (`12a0b2f`) — **the receiver that is not a value**.
An exploded binding now survives being a spliced call's receiver, and a call
whose callee always constructs its result is a fresh initializer, so `var it =
xs.iter()` is three slots and `it.next()` costs **7.7ns/elem** over the plain
`for` where this direction began at 77.5. 708 tests, clean under debug,
`--gc-stress` and `make sanitize`. Everything lands on `main`; there are no
feature branches.

**Nothing is blocked, and nothing is owed.** Milestone 95 closed the last entry
that was not a matter of appetite, and 103 the last one that was already
*designed*. Every item under "Next" is a choice.

**Item 1 of "Next" — the iterator, and the compiler work behind it — is
spent**, at milestone 118. Ten milestones took `it.next()` from +77.5ns/elem
over the plain `for` to +7.7, and the direction has no step left in it; its
entry keeps the reasoning because each finding outlived its milestone.
**Everything that remains is breadth**, so the three below are all appetite:

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
that recorded them. Milestones through 99 live in `history/`.

## Done

Milestones **through 54** are in `history/done-through-m54.md`, **55-75** in
`history/done-m55-m75.md` and **76-99** in `history/done-m76-m99.md`, split out
to keep this file small. Everything from 100 on is below.

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

- **108. `Bytes`, and the first thing a program can read.** (`88aba30`)
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

- **109. A fixed-arity aggregate carries its values** (`f45f176`) — a tuple, a
  struct instance and an enum instance hold their values inside their own
  allocation as a flexible array member, so constructing one calls the allocator
  once instead of twice.
  Design: `runtime.md` "A fixed-arity aggregate carries its values" + "The cost
  model". **THE FINDING: the `Some(x)` that milestone 108 measured at 60.6ns was
  two allocations, and the roadmap had recommended a representation change to fix
  what was really a layout one.** `heap_enum` built the fields array and then the
  object pointing at it, so every `Some`, every struct literal and every tuple
  paid two mallocs, two sweeps and two entries on the all-objects list. The
  eligibility rule is *growable versus not*: a tuple's length is part of its type
  and a struct's field count comes from its declaration, so neither can grow,
  while an `ObjArray` is disqualified by `heap_array_reserve` moving its buffer.
  **SECOND FINDING: folding the values in makes initializing them provably dead
  code.** Each constructor has exactly one caller — `OP_TUPLE`, `OP_STRUCT`,
  `OP_ENUM` — and each fills every slot from the stack before anything can
  allocate, so no collection ever sees a raw one; the `val_unit()` pre-fill was a
  wasted `Value` write per field, and deleting it is a third of the milestone's
  win at arity 2. What replaces it is a `DEBUG`-only poison (an unmappable
  pointer), which costs nothing in release and turns a future mid-fill allocation
  into a crash in the collector instead of a garbage pointer traced in silence.
  Measured, two binaries interleaved, best-of-9 over 3,000,000 elements, as ns
  per element over each binary's own `for` baseline: `it.next()` +81 → **+61**, a
  `Some(x)` minted and matched +70 → **+52**, a 2-field struct literal +59 →
  **+35**, a 2-tuple +61 → **+34**, and the no-aggregate control +7 → +4. **The
  win grows with arity** (a per-object cost plus a per-field write), but **the
  shares do not move** — the `Option` is still ~85% of an iterator's overhead, so
  item 1's remaining steps keep their ranking against a lower ceiling. Not a
  reader changed anywhere: `e->fields` is the same `Value *` after array decay,
  so all 107 use sites compiled untouched. No opcode, no image change. Sabotage
  12/12 attempted, 10 bit — the two no-ops are honest, one being the deleted
  pre-fill (dead by construction, and the poison that replaced it *does* bite,
  though only under `--gc-stress`) and the other milestone 67's singleton, whose
  sharing the language cannot observe by design. New: an assertion in
  `heap_destroy` that `bytes_allocated` is back to zero, which is what checks
  that `obj_size` reports the size an object was allocated with now that only its
  arity records it. Remainder: the values still cost a `Value` each, so the
  representation question item 1 opens is untouched.

- **110. A one-field variant needs no object** (`71d2684`) — a variant with exactly
  one field whose kind fits a machine word becomes a `VAL_VARIANT` — the variant
  pointer plus the field's bits, inside the `Value` — so `Some(x)` stops
  allocating.
  Design: `runtime.md` "A one-field variant needs no object" + "The cost model".
  **THE FINDING: privileging `Option` would have been more code than not
  privileging it.** The roadmap asked for a non-allocating `Option`; what the VM
  needs to answer `OP_TAG` and `value_print` for an inline value is the variant's
  tag and name, and an `Option`-only representation would have had to carry a lang
  item into the image format to get them. A `VariantDef *` carries both already —
  so the general version needs no new opcode, no codegen change, no image change,
  and one branch in `OP_ENUM`. A user's enum gets the same win measured to the
  nanosecond, which is the check that nothing is special-cased. Legal because a
  variant has **no identity the language can observe**: the checker refuses `e.0`,
  so `OP_FIELD_SET`'s `OBJ_ENUM` arm is unreachable from source and a pattern
  binds rather than aliases — m67's fieldless singletons are the same argument one
  field down. **SECOND FINDING, and the expensive one: the field's kind cannot go
  in the four padding bytes beside `Value.kind`.** A member there is a member
  every other `val_*` initializer leaves implicitly zero, and GCC answers that by
  SSE-clearing all 24 bytes then overwriting three quarters of it scalar-wise —
  `for x in xs { sum += x; }`, with no variant in it at all, went 123ms → 185ms
  over 3M ints, +8% instructions but **+55% cycles** (store-forwarding stalls).
  Found only because the harness reports each binary's own `for` baseline, which
  is the thing that had no business moving. The kind lives in the variant
  pointer's three spare low bits instead, so every existing `Value` is
  byte-for-byte what it was; `VAL_VARIANT` being last in `ValueKind` is what keeps
  a field's kind under 8, pinned by a `static_assert` either side and a `DEBUG`
  assert on the alignment. Measured, medians of three interleaved runs agreeing to
  ±2: a `Some(x)` minted and matched +54 → **+38**, `it.next()` +61 → **+51**, a
  user enum's one-field variant +54 → **+38**, its two-field variant +55 → +56,
  struct and tuple literals unmoved. Sabotage 12/12
  attempted, 12 bit — and one bit **only** under `--gc-stress` (dropping the
  collector's trace of an inline field passes 677/677 plain and segfaults under
  stress), which is milestone 109's per-gate lesson repeating. Nothing the
  language does changed: `print` output, equality and every test are identical.
  Remainder: a field too wide for a word still allocates — a `range` payload and a
  nested `Some`, the second of which is what bounds the nesting.

- **111. An item nothing reaches** (`5eb8247`) — the `unused_item` lint: a private
  `fun`, `struct`, `enum` or `trait` that no live declaration names.
  Design: `language.md` "An item nothing reaches", `architecture.md` "An item
  nothing reaches (`unused_item`)".
  **THE FINDING: a private item's audience is its own module, which is what
  makes the lint affordable.** Every other spelling of the question is a
  whole-tree walk after every module is checked — but nothing outside a module
  can name a private item, so the graph never leaves one, and the report lands
  beside `tc_report_unused_imports` with no pass of its own. The same sentence is
  why `pub` is a *root* rather than a question: it moves the audience out of
  reach, so answering it is the remainder rather than the feature. **SECOND
  FINDING: an item cannot be answered the way a binding is.** Milestones 92 and
  107 mark an entry read and report the unmarked ones, and that grain says
  nothing about a function called only by a dead one, or two that call each
  other. So this records a graph — `(from, to)` declaration pairs, `from` being
  whichever declaration the resolve and check loops are on — and walks it from
  the roots to a fixpoint, which is the first analysis in the compiler that is
  not a single pass. An `impl` block is not an item and gets no root: it is
  exactly as alive as the type it extends, so a dead struct's methods die with
  it, while an impl on a builtin is a root because this module cannot see who
  reaches it. Its first act was to report **11 test files**, every one of them
  right — declarations written to type-check, and two `secret`s written to be
  private — which is milestone 105's finding a third time; `@allow` where one or
  two items are inert, `#! flags: -Wno-unused_item` where the declarations *are*
  the test. **std needed no change at all**, which is the evidence that the eight
  resolution sites that mark are the whole of how a name reaches an item.
  Sabotage 20/20 attempted, 19 bit; the one no-op is the **lang-item root**,
  subsumed because every `@lang` item in std is also `pub` — and it is a *live*
  branch, not dead code: dropping `pub` from `pad_center` makes it fire, which
  is how the no-op was told apart from a test gap. Remainders: a `pub` item is
  never reported (**closed by 112**); a method is not an item (it has no
  visibility of its own); and an item reached only from a dead one is reported
  *with* it rather than after.

- **112. A program's tree is its whole audience** (`69a007c`) — `unused_item` now
  reports a `pub` item as well, and the walk it needs became tree-wide.
  Design: `language.md` "An item nothing reaches", `architecture.md` "An item
  nothing reaches (`unused_item`)", `overview.md` "Pipeline" (a sixth phase).
  **THE FINDING: the two halves of the audience question have opposite answers,
  and what decides them is what the build unit is.** A *library*'s `pub` item is
  read by a program that may not be written yet, so it stays a root; a
  *program*'s is not, because its tree is the whole build and nothing outside
  can name even its root module. So `pub` in a program is an ordinary edge, and
  adding it no longer silences the lint — which was the hole milestone 111 left.
  **SECOND FINDING: `@allow` had to become a root, not only a silence.** The
  first program the new rule ran on reported two items the reader had already
  vouched for one edge away — an allowed `pick` still left `b` and `Event` dead
  — so the attribute now seeds the walk, which is what makes it compose. It also
  *deleted* code: with an allowed declaration live, the `diag_push_allowed` at
  the report site can never suppress anything, and a sabotage proved that before
  the argument did. The graph moved from `Module.item_uses` to one list on the
  checker and `Decl.item_live` replaced the index arrays, so an impl's ownership
  became an ordinary **edge** (owner → impl) rather than a case in the walk:
  `decl_index` and the `governs` array are gone. **Every cross-module route
  turned out to be the list milestone 92 already drew** — the four places an
  import binds a name. `link_copy_entry` copying `item` beside `origin` is one
  line and covers `use a::Foo;`, a glob and any `pub use` chain; a bare variant
  reaches its enum through a new `EnumDef.decl`; `a::Foo` is
  `module_export_lookup`'s single caller. Sabotage 19 attempted, 16 bit, and
  each of the three no-ops has an argument: the **pattern** mark on a bare
  variant is redundant rather than dead (a value must be *constructed* to be
  matched, and the constructor sites already mark), the `@allow` push is the one
  above (deleted), and the default that keeps a non-item declaration live guards
  a state nothing records an edge from. The suite gained `#! flags:` in the
  **run bucket**, since `tests/run/unused_defs` is 259 `pub` functions whose
  whole point is that nothing reaches them. Remainder: a method is still not an
  item, and cannot be until method visibility exists.

- **113. A call is a body written somewhere else** (`e66181b`) — a small definition
  is now compiled into its caller rather than called, at a call by name, a
  method call, and the `next()` a `for` drives.
  Design: `runtime.md` "Inlining" and "The cost model" (a fourth table).
  **THE FINDING: the context a spliced body needs is the queue entry, which
  already existed.** It must be compiled under the *definition's* instantiation,
  impl set and depth rather than the caller's — and those three are exactly what
  an `Instance` carries, because they are what `compile_fun_body` takes. So
  inlining is a queued instance compiled into someone else's chunk instead of
  its own, and a *generic* callee needed nothing extra: `mono_request` had
  already keyed the copy by its type arguments. Two more pieces cost nothing
  because earlier milestones had paid for them. **Milestone 88's "a local's slot
  is a stack position" means there is no prologue at all** — the arguments a call
  site pushes already sit where the parameters go, so naming them is the whole of
  it and not one instruction moves. **Milestone 98's landing pad means an inlined
  `return` is a `break`**: same frame, same jump list, same slide, and `?` leaves
  by the same door.
  **SECOND FINDING, and it moves the ranking: a frame is cheap, and what a call
  costs is opcodes.** Milestone 108 attributed +4.6ns to "a call frame"; deleting
  the frame recovered under half of it, because a call by name is four dispatches
  and a splice removes two while adding one. `it.next()` went +45.6 → +41.9
  (8%), a two-stage chain +103.6 → +93.3. The three dispatches left — reading
  back a parameter already in place, sliding it over itself, and a jump to the
  instruction after next — are all a **peephole's**, which is what item 1's
  shortcut now names, and none of them needs an IR.
  Both columns come from one binary, so the interpreter is the same machine code
  and the control cannot drift. Sabotage 17 attempted, **10 bit and 7 were
  no-ops** — the highest proportion yet, and each has an argument, three of them
  worth keeping: swapping the impl set does nothing because a caller always
  imports the callee's module, so its set is a *superset* and import-time
  coherence makes a superset select identically; severing `cg->loop` does nothing
  because the checker has already refused a `break` whose loop is not inside the
  body; and the depth cap, not the recursion check, is what makes a recursive
  call terminate. **One no-op changed the design**: refusing a body that contains
  a closure turned out to be unnecessary, so the refusal was lifted and the rule
  `compile_closure` already stated was generalised instead — a body compiled more
  than once needs a `FunDef` per compilation, and a splice is now the second
  thing that compiles one more than once.
  Remainders: the callee is reached and slotted *before* the decision to splice
  it, so it still gets a chunk of its own that is dead when every call site
  inlined (bytecode grew 1.7-2.8x); and a spliced frame is missing from a runtime
  error's trace, which is the one place inlining is observable.

- **114. The three dispatches a call had left** (`a2b871d`) — the peephole milestone
  113 named: a read the slide is about to remove, and a landing pad reached by
  falling into it. Plus `bench/` and `scripts/bench.sh`, so the cost model's
  tables stop being re-derived once per performance milestone.
  Design: `runtime.md` "The peephole" and "The cost model" (a fifth table),
  `overview.md` repo layout.
  **THE FINDING: the control row is not smaller, it is gone — and that is a
  diff, not a measurement.** `bench/iter/00_for.dt` and `bench/iter/10_call.dt`
  now compile to the same `main` chunk byte for byte apart from one global
  index, so a call whose body is one parameter read costs *literally* nothing
  over writing the body out. Timing only confirmed it (+3.6 → +0.9ns/elem,
  baselines 0.4% apart). Two rewrites, both local: `emit_slide` unemits a read
  of the lowest slot it is about to remove, because the copy and the original
  are the same value and the original is already where the slide would leave it;
  and a body whose last statement is a `return` leaves through a jump that *is*
  the fall-through, so `compile_block` skips its dead epilogue and
  `cg_inline_body` unemits the jump. Neither is about inlining — the first fires
  for any block whose tail expression is its own first local.
  **SECOND FINDING: what makes a backward rewrite safe is one number.**
  `patch_jump` records the only position it ever targets, and `last_target ==
  count` is the whole guard: where two arms of an `if` end in the same read, the
  merge point is exactly the byte the fold would unemit. Sabotage 5 attempted,
  3 bit and **2 were no-ops** — checking that the last instruction really is the
  pad's own jump cannot be made to matter, because every other unconditional
  exit a spliced body can emit is followed by the patch or the back edge of the
  construct that owns it; and skipping a *live* tail expression after an exit is
  sound too, but it would swallow the codegen diagnostics inside it.
  **On the honest result:** only the control row moves. A real `it.next()` gives
  up one dispatch of five opcodes plus an allocation, which is under the
  harness's ~±1ns/elem floor — the rewrite pays where a body *is* its return
  value, and the smaller the body the larger the share.
  Remainders, both now warts: an argument that is already a local is still
  copied into the parameter's slot, and the fold reaches only the first
  parameter.

- **115. A control-flow graph, and a store that no longer hides behind its
  binding** (`c90ac64`) — `unused_assignment` now reports each dead *store*, over
  liveness on a graph built per body (`src/cfg.c`).
  Design: `architecture.md` "Liveness, and the store grain", `language.md`
  "Statements and blocks" + "Warnings and `@allow`", `overview.md` phase 5.
  **THE FINDING: the graph's prerequisite was a soundness bug, not a data
  structure.** Liveness rests on "the binding the checker resolved is the one
  codegen will read", so the pass records the `VarEntry *` each name landed on
  rather than re-resolving — and recording it showed the two did not agree.
  `vscope_find` scanned a scope *forwards*, so `var x = 1; var x = "two";` typed
  the later `x` as `int` while codegen read the later slot: `x + 1` type-checked
  and the VM added 1 to a string pointer. One reversed loop fixes it, and
  `ValueScope` had to allocate entries singly for the recorded pointers to
  survive the array growing. **SECOND FINDING: the exemptions carry the design,
  not the dataflow.** Backward liveness to a fixpoint is textbook; what decides
  whether the lint is usable is that a binding nothing reads *anywhere* keeps
  its single report at the declaration (one mistake, one message), that a
  captured binding is excluded because an upvalue is shared — found on the
  parent graph's chain, so a closure needs no walk of its own — and that a
  compound `a op= b` is a store here where `write_target` calls it a read, which
  is the whole of why `counted += 1` alone is now reported. The events are
  `(kind, local, span)` and nothing else, so the graph knows control flow and
  the AST keeps the data. Its first act was to report **4 test files, every one
  of them right** — a `+=` before an unconditional `break`, a store written to
  prove it is unobservable, and two dead initializers — and **std needed no
  change at all**, milestone 107's result a second time.
  **THIRD FINDING: a match's arms are a chain, not a fan.** Hanging every arm
  off the subject reads as the obvious shape and is wrong: an arm is reached
  because every arm above it declined, and an arm declines in *two* places — its
  pattern did not match, or its guard said no after the bindings were made.
  Without that second edge a store in an earlier guard is invisible to the arms
  below, which is a false positive rather than a missed one, and
  `tests/run/match_guard_flow.dt` is the program that showed it.
  Remainder: escape analysis is the other named consumer and is not built.
  (Milestone 117 built it, and it asked the graph nothing.)

- **116. The variant a match never built** (`18487b1`) — a `match` whose subject
  the code above it constructed is entered by tag: the construction, the tag
  test and the field read all go, and `it.next()` costs 17ns/elem over the
  `for` where it cost 48.
  Design: `runtime.md` "Threading a match into its constructions", the new
  cost-model table, "Match compilation", "Inlining".
  **THE FINDING: the per-path fact needs no analysis, because a path can be
  asked while it is still the last thing emitted.** This file had the item down
  as a forward dataflow over milestone 115's graph — prove *every* path into
  the scrutinee pushes a construction, and of which tag. It is the wrong
  question twice over. A path does not have to be *found* to be a construction:
  at the moment it leaves it is four bytes behind the cursor, which is
  milestone 114's `last_target` argument applied to `OP_ENUM` instead of
  `OP_GET_LOCAL`. And nothing has to hold for *every* path: the paths that
  cannot report their tag meet a **merge** that keeps the tag test they always
  had, so the analysis degrades per path rather than all at once — which is why
  `Take::next`, half of whose exits are its source's answer, threads the half
  it can. The graph is not consulted and did not need to be.
  Second finding: **the slot's contents are decided by the variant's arity, and
  that is what lets two code paths agree without communicating.** A one-field
  variant is unemitted so the field lands in the subject's hidden local; every
  other arity keeps its instance. The threaded entry and the merge each compute
  that from the `VariantDef` alone.
  Sabotage 14/12 bit, and the three that did not each paid: two were no-ops
  first and named a real test gap (nothing merged two *different* constructions
  into one value — `last_target`'s whole job — and the "guard" in the new test
  was an `if` in an arm body, not a guard), and the third found a **live bug**:
  the first splice inside a subject is not the subject's, since `wrap(next())`
  compiles the argument first, so `wrap`'s body was being skipped entirely.
  Claims are now armed by the site that knows which call node it is compiling,
  and identified by a serial rather than the pad's address. The remaining no-op
  is honest: skipping a dead splice epilogue is code size, not behaviour.
  Remainder: a two-field variant still allocates (only a one-field one is
  unemitted), a subject that is a block or an `if` rather than a call arms
  nothing, and `CG_THREAD_OTHER` in a `match` is now unreachable — exhaustiveness
  covers every tag an arm-only match can produce — so the trap it patches is a
  net with no test behind it.

- **117. The struct nothing else can see** (`15b2f0f`) — a `var` bound to a
  struct or tuple literal and mentioned only as `x.f` is compiled as its fields:
  no construction, each field a slot, a read or write an `OP_GET_LOCAL`/
  `OP_SET_LOCAL`. A 2-field struct literal costs 7.8ns/elem over the plain `for`
  where it cost 36 and a 2-tuple 7.3 where it cost 39 — ~80% off both, and the
  allocation is simply gone.
  Design: `runtime.md` "Escape analysis and scalar replacement" and the cost
  model's last table. **THE FINDING: the escape question is SYNTACTIC, and the
  graph is not wanted after all.** An aggregate is a handle, but there are no
  borrows to take and no address to pass, so a *bare mention* is the only way
  one reaches anywhere else — `x.f` is the one shape that keeps a value in, and
  everything else escapes. So this is one walk asking "is every mention a field
  access", sharing `scan_expr` with the inline-cost client, and the def-use
  chains this file spent two milestones promising it would need were never
  needed. Two other things fell out: **inside a closure even `x.f` escapes**,
  since a capture is by slot and an exploded binding is several; and **one
  `locals` entry per slot** (the first named, the rest anonymous) is what let
  every scope's pop count stay a difference of `local_count`, so no other
  bookkeeping in codegen learned that a binding can be more than one slot.
  Sabotage 13/13 bit on the ten that were real; **three were no-ops**, and each
  named its own boundary rather than a test gap — the `arity < 1` early-out
  returns what the walk would have anyway, `locals_base` cannot matter where a
  `VarEntry` is matched by identity, and the `dyn` guard is reachable only for a
  `dyn` local nothing uses, since the one thing a `dyn` does is take a method
  call. Remainder: a receiver escapes, so an iterator does not explode — see
  item 1.

- **118. The receiver that is not a value** (`12a0b2f`) — an exploded binding
  survives being a spliced call's receiver: the splice binds `self` onto the
  caller's field slots and pushes nothing, so `self.front += 1` writes the slot
  the loop reads. With that, a *call* whose callee always constructs its result
  counts as a fresh initializer, the other half `var it = xs.iter()` needed.
  `it.next()` costs 7.7ns/elem over the plain `for` where it cost 15.8, and 77.5
  when this direction opened.
  Design: `runtime.md` "Escape analysis and scalar replacement", extended, and
  the cost model's last table. **THE FINDING: `self` is not a parameter here, it
  is an alias, and the way to say so is to give it no slot of its own.** Naming
  it at the group's base like any other parameter breaks every pop count
  measured from the first one — `cg_close_scope` starts at
  `locals[break_base].slot`, which becomes the *caller's* frame once `self`
  claims a slot below the splice. Leaving `self` out of `locals` entirely and
  carrying it as `self_slot` + `self_field_slots` is smaller and is also the
  truth: those slots belong to the frame the body was spliced into. Second:
  **freshness through a call is a one-exit rule, and the exit has to be the
  construction** — a callee that binds the object to a name first could have
  handed it to something else on the way out. Third: the declaration's inline
  choice and the call's can disagree, and only through the frame-fit test, so
  the cold path rebuilds the object for the call and reads its fields back
  afterwards; a struct is a handle, so the callee's writes land in that object
  and the read-back returns them to the slots. Sabotage 12/12 bit after one
  round trip: **"more than one exit is fine" survived first and was a live
  miscompile**, not a boundary — a callee returning `self.kept` on one path and
  a construction on the last was exploded on both, and
  `tests/run/explode_receiver_escapes.dt` pins it now. The one honest survivor
  is "never collect the calls", which only turns the optimisation off.
  Remainder: `it.map(f)` is unmoved at +39, because a chain's source is stored
  into the adapter and so leaves whole; and a plain `fun` call is never a fresh
  initializer, since `cg_static_callee` reads only a method call's resolution.

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
  "print the frames and stop"

### The test suite

- **the failure path of `io_read_file` is only half testable.** A read that
  fails partway restores the caller's buffer, and the suite cannot build one:
  the reachable failures (a missing file, a directory) append nothing before
  they fail, so the restore is a no-op wherever it can be observed. The
  `ferror` branch itself is tested — a directory opens and then refuses
- `make sanitize` is a separate run, not part of `make test`: a sanitizer
  finding is not a test failure, so the suite stays green while the binary is
  doing something undefined. Nothing enforces that it is run before a commit
