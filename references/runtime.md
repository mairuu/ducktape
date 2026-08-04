# ducktape — runtime (codegen + VM)

`./build/ducktape --run file.dt` compiles the checked AST to bytecode and
executes `main()`. Entry: `compiler_execute` (`src/compiler.c`) → `exe_assign_tags`
then `mono_seed(main)` and drain (`src/codegen.c`) → `vm_run` (`src/vm.c`). `--gc-stress`
collects on every heap allocation instead of on the usual size threshold —
useful for shaking out missed GC roots; not part of `make test` since it's
too slow to run every time.

## Values (`include/value.h`)

Tagged union. Everything but `VAL_OBJ` is a plain value type; `VAL_OBJ`
points at a GC-managed heap object (`include/object.h`):

| Kind | Payload |
|---|---|
| `VAL_INT` | `int64_t` |
| `VAL_FLOAT` | `double` |
| `VAL_BOOL` | `bool` |
| `VAL_CHAR` | `uint32_t` — a Unicode scalar value |
| `VAL_UNIT` | — |
| `VAL_RANGE` | start, end (`int64_t`), inclusive flag |
| `VAL_FUN` | `FunDef *` (top-level function or method — a callable with no captures) |
| `VAL_OBJ` | `Obj *` — string, array, tuple, struct, enum instance, or closure, see "Heap & GC" |

A `Char` is a plain value, not a heap object, which is the whole of its cost in
the runtime: no allocation, no interning, no GC involvement, and `==` is a
comparison of two `uint32_t`. It is stored decoded — a scalar value rather than
its UTF-8 bytes — so the encoding appears only at the two edges where a Char
meets a String: `value_print`/`stringify` on the way out (`utf8_encode`), and
`string_char_at`/`string_prev_boundary`/`strbuf_push_char` in the native
registry (`string_char_count` validates without producing a Char). Both go
through `string_utils.h`, which validates strictly: an overlong encoding, a
surrogate half and anything past U+10FFFF are rejected rather than round-
tripped, so a `VAL_CHAR` that exists is always encodable and no output path
needs a failure case.

`value_equal` (`src/value.c`) is the one equality used by `OP_EQ`/`OP_NEQ`:
strings compare by pointer (interning makes that correct); arrays, tuples,
structs, and enum instances compare structurally (elementwise, recursive —
enum instances additionally compare unequal if their variant differs).
`value_print` (used by `print`, and by nothing else — a string interpolation
segment either renders as a primitive or is a `Display::to_string` call, see
`architecture.md` "Interpolation and `Display`") prints
tuples as `(a, b)`, structs as `Name { field: v, ... }` or `Name(a, b)` for
tuple structs, and enum instances the same way but with the variant's name
and no enum qualifier (`Some(1)`, not `Option::Some(1)`), matching how Rust's
`Debug` prints enums.

Floats go through `value_format_float` (`src/value.c`), which both
`value_print` and the VM's `stringify` call so `print(x)` and `"{x}"` never
disagree. It picks the fewest significant digits that `strtod` reads back as
the same double, then chooses notation: plain decimal while the exponent is in
`[-5, 17)`, scientific outside it, and a `.0` suffix whenever neither a point
nor an exponent survived — otherwise `1.0` would print as `1` and be
indistinguishable from the `Int`. `NaN`, `inf`, and `-inf` are spelled out.
The scanner accepts exponent literals (`1e+18`), so every form printed is one
the language can also read.

## Bytecode (`include/chunk.h`)

A `Chunk` per function (`FunDef->chunk`, arena-allocated): growable code
bytes + a growable constant pool.

**Operand width follows one rule: an operand is one byte when it indexes a
*frame*, two when it indexes a table the *program* grows.** That line is
structural rather than a budget. The VM reserves `STACK_HEADROOM` (256) slots
for a single frame, so a u8 local or upvalue index *is* the frame size —
widening it would address stack that cannot exist, and `CG_MAX_LOCALS` matches
it exactly. A global, struct, enum, vtable or constant index has no such
backing bound: it grows with the program, so it gets two bytes (u16,
big-endian, like the jump offsets), capped at `SLOT_MAX` = 65536.

A third group sits with the frame operands for a different reason: an index
bounded by one *declaration* — a field (`OP_FIELD_GET`), a variant tag, a
vtable method (`OP_DYN_METHOD`) — stays u8, because the parser already caps
and diagnoses those counts well below 256.

Operands are u8 unless noted.

| Ops | Notes |
|---|---|
| `OP_CONST` (u16) `OP_UNIT` `OP_TRUE` `OP_FALSE` | push |
| `OP_POP` `OP_POPN n` `OP_SLIDE n` | SLIDE drops n values *beneath* the top — how a block discards its locals while keeping its tail value |
| `OP_GET_LOCAL` `OP_SET_LOCAL` | frame-relative slot; SET peeks (assignment is an expression) |
| `OP_GET_GLOBAL` (u16) | pushes `VAL_FUN` — one program-wide slot space over every module's top-level funs and impl methods: `exe->globals[slot]` (see "Linking") |
| `OP_CLOSURE const(u16), n` `, then n×(is_local, idx)` | reads the closure's `FunDef` from constant `const` (a `VAL_FUN`), builds an `ObjClosure` capturing `n` upvalues, pushes it — see "Closures & upvalues" |
| `OP_GET_UPVALUE` `OP_SET_UPVALUE` | index into the running closure's upvalue array, reading/writing through the (open or closed) cell; SET peeks |
| `OP_CLOSE_UPVALUE slot` | closes every open upvalue whose stack slot is at/above `base + slot`, right before those locals are popped |
| `OP_ADD/SUB/MUL/DIV/MOD` | int×int stays int; any float widens (matches checker); div/mod by zero int → runtime error; float mod = `fmod` |
| `OP_NEG` `OP_NOT` `OP_EQ/NEQ/LT/LTEQ/GT/GTEQ` | comparisons widen like arithmetic |
| `OP_CAST_INT` `OP_CAST_FLOAT` | dynamic: no-op if already the target kind |
| `OP_RANGE incl` `OP_RANGE_START` `OP_RANGE_TEST` `OP_RANGE_STOP` | build range from end/start; TEST pops i+range, pushes `i < end` (or `<=`); STOP pops range, pushes the first Int past the end (`end`, or `end + 1` when inclusive — saturating at `INT64_MAX`), which is what folds the two spellings into one bound for `std::iter`'s `RangeIter` |
| `OP_JUMP u16` `OP_JUMP_IF_FALSE u16` `OP_LOOP u16` | JUMP_IF_FALSE does *not* pop the condition |
| `OP_CALL argc` | callee value sits beneath the args |
| `OP_RETURN` | pops result, tears down the frame (incl. callee slot), pushes result for the caller |
| `OP_ARRAY count` | pops `count` elems (left-to-right order), pushes a new array |
| `OP_INDEX_GET` | pops index, array; pushes `array[index]` (bounds-checked) |
| `OP_INDEX_SET` | pops value, index, array; sets `array[index] = value`, pushes value back (bounds-checked) |
| `OP_LEN` | pops array; pushes its length as `Int` |
| `OP_INTERP seg_count` | pops that many values, stringifies + concatenates them in order, pushes the result string |
| `OP_DUP depth` | pushes a copy of the value `depth` slots below the top |
| `OP_TUPLE count` | pops `count` elems (left-to-right order), pushes a new tuple |
| `OP_STRUCT struct_slot(u16)` | pops `module->structs[slot]->field_count` elems (declaration order), pushes a struct instance — the *same* instance every time when the def has no fields (see "Fieldless defs are singletons") |
| `OP_ENUM enum_slot(u16) tag` | pops that variant's `field_count` elems (declaration order), pushes an enum instance — likewise shared for a fieldless variant, which is what makes `Option::None` free |
| `OP_FIELD_GET index` | pops a tuple/struct/enum instance, pushes its `index`-th field — no bounds check, since the index is always a compile-time-valid constant |
| `OP_FIELD_SET index` | pops value then a tuple/struct/enum instance, writes the `index`-th field, pushes the value back (assignment is an expression) — the mirror of `OP_FIELD_GET` |
| `OP_TAG` | pops an enum instance, pushes its variant tag as `Int` |
| `OP_MAKE_DYN vtable_slot(u16)` | pops a value, pushes it wrapped as a trait object carrying `exe->vtables[slot]` |
| `OP_DYN_METHOD index` | pops a trait object, pushes its `index`-th method *then* the unwrapped receiver — so the `OP_CALL` that follows sees an ordinary callee-beneath-args stack |
| `OP_DYN_IS vtable_slot(u16)` | peeks a trait object, pushes whether it carries `exe->vtables[slot]`. One table per (trait, concrete type), so this *is* the type test `d as? T` compiles to; peeks rather than pops so the value survives for `OP_DYN_INNER` |
| `OP_DYN_INNER` | pops a trait object, pushes the value inside it — emitted only where an `OP_DYN_IS` just proved which type that is |
| `OP_DYN_UPCAST pair(u16)` | pops a `dyn Sub`, pushes the same inner value carrying the `dyn Super` table. The table is found on the one the value already holds (`VTable.upcasts`), since the site knows both traits and no concrete type |
| `OP_MATCH_FAIL` | runtime error "no match arm matched" — a backstop; the checker enforces exhaustiveness, but guards can still fail every arm |

`OP_ADD` additionally handles `String + String` (interned concat) alongside
its numeric cases; the checker guarantees operand kinds so no other opcode
needs a string case.

## Heap & GC (`include/object.h`, `src/object.c`)

`Heap` (one instance, created in `compiler_execute`, threaded through both
codegen and `vm_run`) owns every runtime object. Every object starts
with an intrusive `Obj` header (`kind`, `marked`, `next` — the all-objects
list used by sweep):

| `ObjKind` | Payload |
|---|---|
| `OBJ_STRING` | interned: `len`, cached `hash`, flexible `chars[]` (NUL-terminated) |
| `OBJ_STRBUF` | `len` written bytes inside a buffer of `cap`, `char *bytes` — a growable text buffer, deliberately *not* interned; see "Growing a string buffer" |
| `OBJ_ARRAY` | `count` live values inside a buffer of `cap`, `Value *items` — see "Growing an array" |
| `OBJ_TUPLE` | `count`, `Value *items` — the same shape minus the capacity (a tuple's length is part of its type, so it can never grow), kept as a distinct kind so printing/equality read as a tuple |
| `OBJ_STRUCT` | `StructDef *def`, `Value *fields` (`def->field_count` entries, declaration order — not necessarily the initializer's source order) |
| `OBJ_ENUM` | `VariantDef *variant`, `Value *fields` (`variant->field_count` entries, declaration order) |
| `OBJ_CLOSURE` | `FunDef *fun`, `ObjUpvalue **upvalues` (`upvalue_count` entries) — a capturing function value |
| `OBJ_UPVALUE` | `Value *location` (→ a live stack slot while open, → `closed` once closed), `closed`, `next` (open-list link) |
| `OBJ_DYN` | `Value inner` (the coerced value, stored as-is), `VTable *vtable` — a trait object |

`StructDef`/`EnumDef`/`VariantDef` pointers are arena-owned (live for the
whole compilation), not GC objects, so marking a struct/enum instance walks
its `fields` array but never touches the def pointer itself.

### Fieldless defs are singletons

A `StructDef` or `VariantDef` with `field_count == 0` has **one** instance for
the whole run: `heap_struct`/`heap_enum` build it on the first construction,
file it on the def as `singleton`, and hand that same object back forever after.
`Option::None`, `Slot::Empty` and `struct Marker;` therefore stop allocating.

Nothing in the language can see the sharing, and the two reasons are the whole
argument for it:

- **`==` on an aggregate is structural** (`value_equal` compares the variant/def
  and then the fields), so two separately-constructed `None`s already compared
  equal. Sharing changes how that answer is reached, not what it is.
- **there is no field to write through.** Aggregates are handles — a struct
  passed to a function mutates through to the caller — but `OP_FIELD_SET` needs
  a field index, and a fieldless def has none. There is no state to alias.

Two consequences worth knowing:

- **A generic enum shares one singleton across every type argument.** There is
  one `EnumDef` per source enum (instantiation monomorphises *functions*, never
  defs), so `Option::<Int>::None` and `Option::<String>::None` are literally the
  same object. Types are erased at runtime, so this was already indistinguishable
  — those two values compared equal before the change too.
- **The def is a GC root, not a weak cache.** `heap_collect` marks
  `structs[i]->singleton` and `enums[i]->variants[j].singleton` off `Heap.exe`,
  so a singleton never dies. It has to be this way round: a weak cache would let
  a collection that lands between two constructions free an object the def is
  still pointing at, and the next construction would hand back freed memory.
  (The set is bounded by the source text — one object per fieldless def the
  program actually constructs — so immortality costs nothing that matters.)
  `heap_destroy` unfiles every singleton, because the defs outlive the heap.

A `StringBuf` is the one object defined by what it is *not*: an `ObjString` is
in the intern table, so its bytes cannot change (see below); a buffer is out of
it, so they can. Nothing else about the two differs, and `StringBuf`'s `build`
method is the door — one `heap_intern` of the bytes written so far.

String identity is pointer identity — `heap_intern` looks up an
open-addressing table (FNV-1a hash, linear probing, tombstone deletes)
before allocating, so `==` on strings is a pointer compare. Codegen interns
string *literals* at compile time (`cg_decode_string` in `src/codegen.c`,
decoding the scanner's `\n \t \r \" \\ \{` escapes); the VM interns at
runtime for `+` concat (`heap_concat`) and interpolation (`OP_INTERP`,
`stringify` in `src/vm.c`). `stringify` handles only the four primitives and
passes a String straight through, which is the whole reason `Display` cost the
runtime nothing: the checker rewrites a non-primitive segment into a
`to_string()` call, so by the time `OP_INTERP` sees it, it is a String.

**Interning buys equality and nothing else.** `==` is a pointer compare because
the table guarantees one object per distinct byte string, but pointer *order* is
allocation order — arbitrary, and different between a run and a `--emit-bc`
replay of it. So `string_cmp`, the native under `impl Ord for String`, gets
exactly one shortcut from the table (identical pointers answer 0) and otherwise
walks the bytes: `memcmp` over the shared prefix, then the shorter one first if
that ties. It cannot tie *at equal length* — equal bytes at equal length would
be one pointer — and the result is normalised to -1/0/1 because `memcmp`'s
magnitude is implementation-defined and a program can print what `cmp` answers.
`memcmp` compares as unsigned char, so the order is code-point order for
anything well-formed in UTF-8. Nothing else in the runtime moved: it is a
non-allocating native, so it does not even exercise the calling convention's
rooting rule.

**Collection** is mark-sweep, triggered from `heap_alloc` (the sole entry
point that grows `bytes_allocated`) once it crosses `next_gc` (starts at
1 MiB, doubles `bytes_allocated` after each cycle), or unconditionally under
`--gc-stress`. Roots: every fieldless def's `singleton` (see above) and every
compiled chunk's constant pool, the latter scanned directly
off `Heap.exe`'s `globals[]` *and* `closures[]` (so constants need no special
"immortal" case — each function has its own chunk with its own constants;
nested closure `FunDef`s are in no slot space, so codegen appends them to
`exe->closures` purely to keep their constant pools rooted). Because those
tables are program-wide, a constant interned inside a *dependency* module's
method is rooted just like the root module's — the whole point of linking
before the first chunk is compiled. Plus the VM's live value stack
*and its open-upvalue list*, reached through `Heap.mark_roots` — a
function pointer `vm_run` installs on entry and clears on every return path
(`VM_RETURN` macro), so a collection during codegen (no VM running yet) only
marks constants. The intern table is *weak*: entries unreached by the mark
pass are tombstoned before sweep frees the backing object.

The tombstone count is folded into the same load-factor check that triggers
growth (`table_grow` rebuilds the table and drops tombstones) — tombstoning
must **not** decrement the count used for that check, only a full rehash may
reset it. Getting this backwards was an actual bug during 5b: with tombstones
invisible to the load factor, the table never regrew to purge them, so after
enough collect cycles every slot was tombstoned and a miss in `table_find`
(whose loop only terminates on a `NULL` slot) spun forever.

Any allocating call (`heap_intern`, `heap_concat`, `heap_strbuf`, `heap_array`,
`heap_tuple`, `heap_struct`, `heap_enum`, `heap_closure`, `heap_upvalue`,
`heap_array_reserve`, `heap_strbuf_reserve`) may collect *before* the new
object exists — `heap_struct`/`heap_enum` only when the def has fields, since a
fieldless one allocates at most once per run — so the discipline throughout
codegen/VM is: keep every
already-live operand reachable from a root (still on the VM stack, not yet
popped) until the new object is built and pushed. See the
`OP_ARRAY`/`OP_INTERP`/`OP_TUPLE`/
`OP_STRUCT`/`OP_ENUM` handlers in `src/vm.c` for the pattern (index math into
the stack in place, decrementing `sp` only after the result exists).

### Growing an array

An `ObjArray` holds `count` live values in a buffer of `cap`.
`heap_array` — the only thing `OP_ARRAY` calls, and it already knows how many
elements it just evaluated — builds an exact fit (`cap == count`), so an array
literal costs nothing extra and only `[T]`'s `push` method ever grows one.
`heap_array_reserve` doubles the buffer (from a floor of 8), copies the live
prefix, fills the tail with unit and frees the old buffer through
`heap_dealloc` so `bytes_allocated` stays honest. `free_obj` releases `cap`
values, not `count`: past the first push the two differ.

`count` is the whole of what is live — the mark phase walks exactly
`[0, count)` and nothing reads past it — and that is what fixes the ordering:

- **The new buffer is allocated before anything about the array changes.** The
  collection `heap_alloc` may trigger therefore finds `count` live values in
  the old `items`, which is a consistent array. Both halves matter: the array
  is reachable (its owner keeps it rooted, which for `push` means it is still
  an argument on the VM stack), so the collector *will* walk it, and what it
  walks has to be the buffer that is actually there.
- **`count` rises only once the slot exists.** Raising it first would point the
  collector at a `Value` that has never been written — the same class of bug as
  a `FunDef` missing from `exe->globals` (milestone 8) or `module->methods[]`
  going unscanned (5c-ii): something reachable-looking the collector cannot
  read correctly.

Popping is the mirror. Lowering `count` is what drops the value, so the array
stops rooting it; it survives only because the VM pushes the native's result
with nothing allocating in between — the same window `string_slice`'s freshly
interned result already lived in.

Growth reallocates, so **no raw `Value *` into `items` may be held across a
push**. Nothing in the VM does: `args` points into the value stack, not into
an array's buffer.

### Growing a string buffer

An `ObjStrBuf` holds `len` written bytes in a buffer of `cap`, and
`heap_strbuf_reserve` doubles it from a floor of 16 exactly as the array path
does — allocate the new buffer, copy the live prefix, free the old one through
`heap_dealloc`. `free_obj` releases `cap` bytes, not `len`. An empty buffer has
no allocation at all (`bytes == NULL`), which is why `build` and `value_equal`
both test the length before reading the pointer: neither `memcpy` nor `memcmp`
is defined on a NULL pointer, even for zero bytes.

What is worth comparing is the *ordering rule*, because only half of it carries
over:

- **The new buffer is allocated before anything about the object changes**, and
  the object must be rooted across the call — unchanged. `strbuf_push` gets that
  for free: the buffer is `args[0]`, still on the VM stack, which is milestone
  16's native calling convention doing its job.
- **`len` rises only once the bytes exist** — true here too, but for a *weaker*
  reason. An array's tail is traced, so a `count` raised past an unwritten slot
  hands the collector a `Value` that was never stored: a real crash. Bytes are
  pure payload; `mark_obj` does nothing for an `OBJ_STRBUF`, so all the ordering
  protects is what `build` will copy. That asymmetry is what shows the array
  rule was about the *collector* rather than about the buffer.

`strbuf_push_char` (milestone 26) is the same code with `utf8_encode` in front
of it: one to four bytes into a stack array, then the identical reserve-copy-
advance. That it needed no new rule is the point — a Char's bytes are bytes.
It is also the append an array of parts could never have offered, since there
is no String to hold one character, which is what makes `from_chars` ducktape
rather than a native. `strbuf_push_int` is the third of the same shape —
`snprintf` a signed decimal into a 24-byte stack array (an int64_t is at most 20
digits and a sign, so it cannot overrun), then the same reserve-copy-advance —
and `strbuf_clear` is the one strbuf native that allocates nothing at all: it
drops `len` to zero and leaves `cap`/`bytes` where they were, so the space is
reused rather than freed, and there is no rooting question to get wrong.

Nothing else in the runtime changed for this. A `StringBuf` is never a chunk
constant — there is no literal syntax for one, and only a native can make one —
so the image format has nothing to carry and `src/bytecode.c` is untouched.
`==` is the only place the kinds visibly diverge inside the VM: an `OBJ_STRING`
compares by pointer *because* it is interned, an `OBJ_STRBUF` has to compare
its bytes.

## Codegen shapes (`src/codegen.c`)

- Locals are compile-time tracked (`Cg.locals`, name → frame slot); the
  initializer's stack slot *is* the variable. Params occupy slots 0..n-1.
- **A local's slot is a stack position, not its index in `Cg.locals`.** The two
  agree only while nothing else is pending, and a block is an expression here,
  so a `var` can be declared with an operand, a callee, an array element or a
  compound assignment's duplicated target already beneath it. `Cg.depth` is what
  the frame actually holds — locals *and* those temporaries — and
  `cg_add_local` names the value on top of it (`slot = depth - 1`).
- Keeping `depth` right costs almost nothing per case, because `compile_expr`
  sets it to "one more than on entry" on the way out: an expression leaves
  exactly one value however it got there, so an operand sequence accumulates
  without being asked to and drift inside a construct cannot outlive it.
  `compile_stmt` is the same rule one level up — a statement leaves exactly the
  locals it declared. What is left is the raw traffic *between* expressions
  (`cg_pushed`/`cg_popped`): a condition popped before a body, a callee pushed
  by `cg_emit_target`, an interpolation's literal segments, the hidden locals a
  `for` sets up by hand (`cg_add_pushed_local`).
- A branch target resumes the depth of its **fork**, not of the path that fell
  into it — an `if`'s else arm, each `match` arm, a failed pattern test. Those
  are the sites that save a depth and restore it rather than adjusting one.
- `break`/`continue` unwind to a recorded *depth* (`CgLoop.break_depth`,
  `continue_depth`) rather than to a count of locals, since a loop body can
  reach one with temporaries pushed that are no one's local. The local bases
  stay beside them for `cg_close_scope`, which is a question about locals.
- Because the unwind is measured **from a frame**, a *labelled* break costs
  codegen only the choice of frame: `cg_loop_target` walks `CgLoop.parent` for
  the matching `label` and hands the same four numbers to the same arithmetic,
  so `cg->depth - loop->break_depth` counts everything stacked since *that*
  loop began — inner loops, their locals and any pending temporaries included —
  and `cg_close_scope(loop->break_base)` detaches every capture on the way.
  Nothing else changed: no new opcode, no image change, and the jump is
  recorded on the named frame so it patches at that loop's landing pad.
- A **labelled block** is the same frame with no back edge (`CgLoop.is_block`).
  It costs `compile_block` one frame and one round of patching and nothing else,
  because a block's ordinary exit already leaves the tail at the depth the block
  opened at — which is exactly where a break's slide leaves its value. So the
  landing pad *is* the block's own exit: nothing to emit there, and no jump over
  a back edge because there is none. `is_block` also makes the frame invisible
  to an unlabelled `break`, and that skip has to match `check_loop_target`'s to
  the letter — two resolvers of one name agree only by asking the same question.
  Its `continue_*` fields are never read: the checker refuses a `continue` that
  names a block, which is the only way to reach them.
- The one-byte operand bounds the *slot*, so `cg_add_local` checks that and not
  only the count: temporaries between locals push positions up faster than
  entries.
- Blocks: compile stmts, compile tail (or `OP_UNIT`), then `OP_SLIDE n`; a
  labelled one patches its breaks to land right after that slide.
- `and`/`or` compile to jump-based short-circuit; `if` always produces a
  value (`OP_UNIT` for a missing else).
- `loop { .. }` (`compile_loop`) is `compile_while` with the condition and its
  exit jump removed, so the only edge out is a patched `break` — and since each
  one of those brings the loop's value with it, the landing pad emits nothing.
  The trailing `OP_UNIT` is only for the loop nothing leaves, where it is
  unreachable but keeps "an expression leaves a value" true without a case.
  Divergence is a checker fact and costs codegen nothing.
- A `break` in a `loop` (`CgLoop.break_takes_value`) compiles its value first —
  it may read the very locals about to go — and then `OP_SLIDE`s them out from
  under it, the same "remove n beneath the top" a block's tail already needed;
  a bare `break;` there pushes `OP_UNIT` so every edge into the pad agrees. A
  `break` in any other loop lands where that loop's own exit does, which carries
  no value, so it stays the plain `OP_POPN`. No opcode and no image change:
  `OP_SLIDE` and the existing `break_base`/`cg_close_scope` cover body locals
  and captures, and the close still happens before the slide.
- `while` and `for` evaluate to unit. `for x in <range>` materializes two locals —
  hidden `range` and the loop variable — then tests with `OP_RANGE_TEST` and
  increments by constant 1. `for x in <array>` (`compile_for_array`)
  dispatches on `iterable->resolved_type->kind`; it materializes three
  locals — hidden `arr`, hidden `idx` (starts at 0), and the loop variable
  (starts as unit) — tests `idx < OP_LEN(arr)`, reads `arr[idx]` into the
  loop variable at the top of each iteration, and increments `idx` at the
  continue target. `break`/`continue` emit `OP_POPN` down to the loop's
  recorded local base (continue keeps the hidden iter locals, break does
  not) before jumping; forward continue targets the increment. Both shapes
  survive milestone 60's `xs.iter()` / `(0..n).iter()`: a source is a library
  value that compiles as `compile_for_iter`, and the two desugarings stay the
  direct path they were — which is why `for x in xs` still re-reads `OP_LEN`
  each turn, the behaviour `ArrayIter` then matched deliberately.
- `for x in <iterator>` (`compile_for_iter`, the fall-through when the iterable
  is neither array nor range) materializes two locals — hidden `iter` (the
  receiver) and the loop variable (starts unit) — and each turn calls
  `iter.next()` on the *local* (not a re-evaluated expression, so the cursor
  advances in place). The result is an `Option`, taken apart the way `?` takes
  apart a `Result`: `OP_DUP`, `OP_TAG`, compare to the `Some` tag; on `Some` pop
  the flag, `OP_FIELD_GET 0`, `OP_SET_LOCAL` the loop variable, run the body,
  and loop back; on `None` fall through, pop the flag and the `Option`, and pop
  the two locals. The checker put the resolved `next` call (with its
  instantiation) and the two `Option` variants on the `ExprFor`. Which `next` to
  emit is the same three-way choice `compile_method_call` makes, and it is read
  the same way: a concrete impl method (`resolved_method`), an inherited default
  (`resolved_default`), or **dispatch through a bound** when the receiver is only
  known abstractly — a generic `I: Iterator` (codegen substitutes the receiver
  to a concrete type and re-runs `cg_bound_target`, monomorphising the body) or a
  `dyn Iterator` (`OP_DYN_METHOD` picks the slot off the vtable). The last two are
  what let `for` drive a value whose iterator type the loop cannot see; milestone
  46 emitted only the concrete case. `continue` is backward (it re-drives
  `next()`), like a `while`.
- Calls: push callee (`OP_GET_GLOBAL` or a local), args left-to-right,
  `OP_CALL`, whose callee may be a ducktape function (opens a frame), a
  closure, or a native (runs C in place). A call to an `@intrinsic` is not a
  call at all: it lowers to that opcode inline.
- Strings: literals intern directly to a constant (`cg_decode_string`);
  `EXPR_INTERPOLATED` pushes each segment (text segments as string
  constants, expr segments compiled normally) then one `OP_INTERP`.
- Arrays: `EXPR_ARRAY` compiles elements then `OP_ARRAY`; `EXPR_INDEX` is
  object-then-index-then-`OP_INDEX_GET`. Assigning to `arr[i]` (plain `=`)
  pushes array, index, value, then `OP_INDEX_SET`; a compound `+=` etc.
  additionally duplicates array+index to read the current value before
  combining (`compile_index_assign`). The DUP depth is 1 for the opcode form
  and 2 when the operator is a trait call, because that call's callee has been
  pushed in between — see "Compound assignment" below.
- Tuples: `EXPR_TUPLE` compiles elements then `OP_TUPLE`.
- Structs/enum variants (`compile_struct_init`/`compile_variant_init`): the
  checker validates field names/arity/types but doesn't reorder a literal's
  fields to declaration order, so codegen does — for each field in
  `StructDef`/`VariantDef` order, it scans the initializer's `FieldInit`s for
  the matching name (or index, for tuple structs/variants) and compiles
  *that* value, then emits one `OP_STRUCT`/`OP_ENUM`. This guarantees the
  compiled value order always matches the runtime instance layout regardless
  of source order.
- Field access (`EXPR_FIELD`, both `.name` and tuple `.0`/`.1`): the checker
  resolves and caches the field's declaration-order index on the expr node
  (`resolved_index`); codegen just compiles the object then emits one
  `OP_FIELD_GET`.
- Assigning to `obj.field` (`compile_field_assign`): the plain `=` compiles the
  receiver then the value, then one `OP_FIELD_SET index`; a compound `+=` etc.
  compiles the receiver once, `OP_DUP`s it to read the current field
  (`OP_FIELD_GET`), combines, then `OP_FIELD_SET`. The receiver expression is
  evaluated once — the same discipline `compile_index_assign` keeps for
  `arr[i]`. Every aggregate the getter reads is writable (struct, tuple element,
  enum-variant field); the assignment yields the written value, so it is an
  expression like every other. There is no `mut` marker and no aliasing check:
  a struct is a shared heap reference, so a write through one binding or through
  `self` is visible through every alias — the same rule arrays already keep.
- Compound assignment (`cg_compound_begin`/`cg_compound_end`), shared by all
  three places above: see "Compound assignment" below.
- Match (`compile_match`, see "Match compilation" below), and the two
  conditional bindings that reuse it (`compile_if_binding`,
  `compile_while_binding`).
- Methods (`compile_method_call`) and associated functions/bare
  self-supplying calls (`Point::new(...)`, `Shape::area(s)`): see "Methods"
  below.
- `?` (`compile_propagate`): see "Propagate (`?`)" below.
- Closures (`compile_closure`): see "Closures & upvalues" below.
- Generic functions, methods and impls (`compile_fun_body` under a `Subst`):
  see "Monomorphisation" below.
- Destructuring `var` bindings (`compile_destructure`): see "Match
  compilation" below.
- Trait default bodies the impl inherited (`compile_method_call` again, via
  `TraitMethodDef.default_impl`): see "Monomorphisation" below.
- Anything outside the subset (an `@intrinsic` named as a value rather than
  called,
  say) emits a source-anchored diagnostic
  `"... is not supported by the VM yet"` and fails codegen — it never
  crashes at runtime.

### Compound assignment

`a op= b` is `a = a op b`, so codegen's job is the *place*: it is compiled once
and then read back off the stack, which is what keeps `xs[next()] += 1` from
advancing `next` twice. The three targets (local/upvalue, field, index) each
keep their own stack discipline and share the operator through two calls.

`cg_compound_begin` runs **before** the current value is read, and
`cg_compound_end` with `[.., self, arg]` on top. The split exists for one
reason: when the checker decided the operator is a trait call rather than an
opcode (`ExprAssign.op_call`, an `a.add(b)`), `OP_CALL` wants `[callee, self,
arg]` — so the callee has to be pushed *underneath* the value the place is
about to yield. That is the only difference between the two forms, and it is
why every `OP_DUP` depth in the three targets shifts by one exactly when a
callee was pushed.

Nothing else is new: the callee is the ordinary `cg_emit_target`, the call is
an ordinary `OP_CALL 2`, and the built-in form emits the same `OP_ADD`/`OP_SUB`/
`OP_MUL`/`OP_DIV`/`OP_MOD` it always did. What changed in milestone 83 is that
the checker now *decides* which of the two it is — those opcodes read their
operands as numbers with no tag test, and a compound assignment used to reach
them without asking.

A `dyn` receiver, an `@intrinsic` body, or a method whose `self` is not the
first parameter would need a different stack shape, so each reports rather than
compiling (none is reachable: no `std::ops` method is object-safe).

### Match compilation

The subject is evaluated once into a hidden local; every pattern is
addressed relative to that local as a chain of field indices (`Accessor`:
base slot + a short path of `OP_FIELD_GET` indices). Each arm compiles in
two passes over its pattern:

- `compile_pattern_test` emits the runtime checks — literal equality
  (`OP_EQ`), enum tag equality (`OP_TAG` + `OP_EQ`) — recursing into
  struct/variant/tuple sub-patterns; wildcard and bind patterns need no
  check. Each check follows the `if`-statement idiom (push bool,
  `OP_JUMP_IF_FALSE` to a per-arm fail list, `OP_POP` on the fallthrough
  side) but relies on exactly one of those jumps ever being taken per arm
  attempt (pattern tests short-circuit), so a *single* shared `OP_POP` after
  the fail list's patch point balances whichever one test failed, before
  falling into the next arm.
- `compile_pattern_bind` — run only once every test has passed — declares a
  local for every name the pattern binds (plain binds, and struct/variant
  field shorthand), each via the same "the pushed value's stack slot becomes
  the local" convention used elsewhere.

Splitting test from bind avoids threading partial-bind cleanup through the
test logic: on failure nothing has been bound yet, so falling through to the
next arm never needs to unwind anything. A guard runs after binding (so it
can reference bound names); on a false guard, codegen pops the guard's own
condition, `OP_POPN`s the locals `compile_pattern_bind` just added, and jumps
to the next arm through a *second* jump list (`next_arm_jumps`) that lands
*after* the shared fail-list `OP_POP` — a guard failure's stack is already
clean, unlike a failed test's.

Falling past every arm hits `OP_MATCH_FAIL`. The checker now enforces
exhaustiveness (`architecture.md` "Match exhaustiveness"), but a guarded arm
cannot count towards coverage, so a match whose only applicable arms are
guarded can still fall through — this stays a real, reachable runtime error.

#### Destructuring a `var`

A `var` binding is that machinery with no arms: `compile_destructure` is the
same two passes over one pattern, against a hidden local holding the
initializer. `var x = e;` keeps the old shape — the initializer's own stack
slot becomes the local, no temp — and every other pattern evaluates the
initializer into an anonymous local that the accessors read from, appending
the bound names above it. The temp costs one slot for the rest of the scope,
which is what keeps each bound name a plain local: assignable, capturable by
a closure, and popped by the ordinary scope-close path.

The test pass runs even though the checker rejects refutable bindings,
because its answer is tri-state — an unsolved column type reports nothing
(`architecture.md` "Binding patterns"). For an irrefutable pattern
`compile_pattern_test` emits nothing at all, so the common case costs zero
instructions; anything it does emit traps through `OP_MATCH_FAIL` rather than
binding names out of a value that never had the shape.

An `else` block is emitted between that shared `OP_POP` and the trap, and needs
nothing else. The bindings have not been pushed at that point, so the stack
holds exactly the outer locals plus the subject, and a `break` or `continue`
inside pops the right number on its way out with no special case. The trap stays *below* the block rather than being replaced by
it: the checker made the block diverge, and the alternative to a trap if one
ever fell through anyway would be reading names that were never bound. Since
milestone 85 checks `-> Never` bodies too, no source reaches it — it costs one
byte and is the only thing standing between a checker bug and unbound reads.

#### `if var` and `while var`

The same two passes over one pattern again, with the fail list wired to
something other than the next arm. Neither needs an opcode of its own.

`compile_if_binding` is `compile_match` with the arm list spelled out: subject
into a hidden local, test, bind, then-block, `OP_SLIDE` the bound names out
from under the result, jump to the end. The fail list patches to the shared
`OP_POP` that balances whichever test failed, and the `else` (or `OP_UNIT`)
follows it; one final `OP_SLIDE 1` drops the hidden subject and keeps the
branch's value, exactly as the match ending does.

`compile_while_binding` differs from every other loop in where the hidden local
lives: the subject is **re-evaluated each turn** — that re-evaluation is what
advances an `it.next()` — so its local is pushed inside the loop and popped
again at the bottom, where `for`'s loop-carried slots are allocated once above
`loop_start`. A failed test *is* the exit, so there is no separate exit jump:
the fail list lands past the back-edge, pops the outstanding bool and then the
subject, and the loop's `break` jumps patch after that. `break_base` and
`continue_base` are both the pre-subject local count, so either statement pops
the bindings and the subject on its way out; `continue` is backward, like a
`while`'s, and re-drives the subject.

Because the bindings are pushed and closed (`cg_close_scope`) inside the loop,
a closure made in the body captures **that turn's** binding rather than one
shared cell — the opposite of `for x in xs`, whose loop variable is a single
slot written each turn (`language.md` "Not yet implemented").

### Methods

Impl methods are ordinary `FunDef`s — `self` is just a regular `ParamDef`
with `is_self` set, at whatever position it was declared (checked
generically, not assumed to be first) — so they compile exactly like
top-level functions (`compile_fun_body`, shared by both).

Codegen reaches a method through the `FunDef` alone, never through a `Decl`:
a `FunDef` carries its own body and span, which is what both monomorphisation
and the reachability walk need — a call site in another module has no `Decl`
to consult. That is also why there is no lockstep pairing between
`impl_decl->items` and `impl_def->methods[]`, which would only be needed
because an impl item's `Decl` is never linked back to its `FunDef`
(`item->fun_decl->as.fun_decl.def` is never set).

Methods and top-level functions share one `OP_GET_GLOBAL` slot space (see
"Linking"), so calling one needs no new opcode:

- `obj.method(args)` (`compile_method_call`): the checker elides `self` from
  `mc->args` (`mc->object` holds it instead) and validates arguments against
  `fun->params` skipping whichever index has `is_self`. Codegen mirrors that
  exactly — interleaving `mc->object` back in at that same position — so
  pushed arguments always land positionally where the callee's calling
  convention expects them, regardless of where `self` was declared.
- `Point::new(...)` / `Shape::area(s)` (bare paths, no dot syntax — either an
  associated function, or a method called with `self` supplied as a normal
  explicit argument): these stay a plain `EXPR_CALL` with a multi-segment
  `EXPR_PATH` callee; the checker caches which `FunDef` the path resolved to
  on the path node itself (`resolved_fun`), and codegen's `EXPR_PATH` case
  just pushes it by slot like any other callable.

### Monomorphisation

The runtime is *uniform* in type arguments. An instance's fields are slots in
declaration order, a variant's tag is per enum, and no opcode inspects a
static type — which is exactly why `?` can propagate an `Err` without
rebuilding it. So a type argument changes one thing and one thing only about
the code compiled from a body: **which function a call resolves to.**

```
fun describe<T: Show>(v: T) -> String { v.show() }
```

`v.show()` is a different body for `T = Int` than for `T = Bool`, and the
receiver is abstract until the call site says otherwise. That is the whole
reason generic code cannot simply be erased, and it also bounds the job:
monomorphisation duplicates *code*, never types. `StructDef`/`EnumDef` and
their slots stay shared across every instantiation.

An instantiation is a `(FunDef, type arguments)` pair; a `Mono` (`Instance[]`
plus a cursor) memoises them and doubles as the worklist:

- A generic definition gets **no slot** — there is no single body to address.
  An uncalled generic therefore costs nothing and is not an error.
- A call site whose target is generic asks `mono_request` for the copy keyed
  by its type arguments. First request allocates a `FunDef` clone (same body,
  params and name; its own slot appended to `exe->globals`, its own chunk)
  and queues it; later requests return the same one, which is what makes a
  recursive generic terminate.
- The queue is drained until empty (`mono_pending_module` / `mono_compile_next`),
  each body reported against the module it was *written* in, not the one that
  reached it. Draining can enqueue more, so it is a fixpoint rather than a
  one-level expansion.

That queue is the program's whole reachability walk, and non-generic
definitions ride it too: an `Instance` with `origin == instance` and an empty
`subst` is a definition with nothing to specialise, which is precisely what a
non-generic one is (`mono_reach`; see "Linking" → "Reachability"). Nothing is
compiled because it was *written* — everything is compiled because something
*named* it.

The type arguments come from the checker: `resolve_callee`,
`resolve_method_call_expr` and `check_trait_method_call` each stash the
`Subst` they solved onto the call node (`ExprPath.inst`,
`ExprMethodCall.inst`), rewritten to concrete types by `cctx_solve_insts`
once `infer_finalize` has run. Those arguments are in the *enclosing*
definition's terms, so a call inside a generic body is instantiated by
pushing the caller's own bindings (`Cg.subst`) through them — the step that
lets `describe_twice<T>` reach `describe<Int>` reach `Int::show` rather than
stopping one level down.

**Pushing them through is two steps, not one**, and `cg_subst` is where both
happen. Substituting binds the type parameters (`I` → `Counter`), but a type
argument may be a *projection over* one — `unwrap<T>` called on the payload of
`it.next()` inside a `fun f<I: Iterator>` is keyed on `T = I.Item` — and
substituting inside the base leaves `Counter.Item` standing, because the binding
lives on an impl. So `cg_subst` follows `subst_apply` with `assoc_apply`, which
reads the binding off the applicable impl (`architecture.md`,
"Associated-type projections"). Codegen has no `InferCtx`, so it cannot borrow
the checker's `infer_apply`; what it does have is the `ImplIndex` the body may
select from, which is all the collapse actually needs.

Without that second step an instantiation keyed on an element type could not be
formed, and the failure surfaced as a diagnostic about a type the impl knew
perfectly well — "cannot instantiate 'unwrap': type argument 'T' is not known
here", at a call the checker had accepted. It is applied at every site that
substitutes rather than at the instantiation path alone, so a `dyn` coercion or
a bound-dispatch target whose type mentions a projection is collapsed the same
way.

Two call shapes need more than the recorded arguments:

- **A method of a generic impl.** Its body mentions the impl's type params as
  well as its own, so `cg_inst_key` binds both, impl's first, dropping any
  the method's own shadow by name — the same rule `subst_exclude_shadowed`
  applies in the checker, and for the same reason (`subst_apply` matches by
  name and takes the first hit).

  Which of the two recorded substitutions an impl parameter is read from is
  therefore load-bearing: a call through a bound records the *trait's*
  arguments, and `impl<T: Tag> Boxed<Int> for T` against `trait Boxed<T>` has
  two live meanings for `T`. An impl parameter is read from the impl match
  first, because nothing else can speak for it; a method's own parameter keeps
  reading the call's arguments first.
- **A call through a trait bound.** The checker resolved `v.show()` against
  the trait *signature*, so there is no `MethodDef` on the node — only
  `bound_trait` (the trait *reference*, so a generic trait's arguments come
  with it) and `bound_self`. Codegen substitutes both into this
  instantiation's terms and re-runs `impl_index_method` to find the body,
  falling back to `impl_index_default_method` when the impl inherited it.
  Passing the reference is what tells `impl Into<Int> for S` from
  `impl Into<String> for S`, which the receiver alone cannot. This is the one
  place codegen consults an impl index — and since impls became
  module-granular, *which* index is a question of its own.

  A path qualified by a type parameter (`T::make(1)`) is the same shape with
  the receiver removed: `bound_trait`/`bound_self` sit on the `ExprPath`
  instead, and both branches call `cg_bound_target`. Nothing else differs —
  an associated function is an ordinary global once its body is chosen.

**The impl set travels with the instantiation, not with the definition.**
`std::cmp::max<T: Ord>` needs `impl Ord for P`, and that impl is written
wherever `max` is called — `std::cmp` itself cannot see it, and must not have
to. So `Instance.impls` records the *requesting* module's `visible_impls`
(seeded from `Cg.impls`, propagated into every instantiation the body enqueues
and into nested closures) exactly as `Instance.subst` records the requesting
module's type arguments. The two travel together for the same reason: a bound's
witness and its type argument are both written at the call site.

This is sound because reachability is transitive. A request that starts in
module M can only reach definitions in modules M reaches, so M's visible set is
a superset of every set the bodies below it could need — and any ambiguity
inside it would already have been refused by the coherence check
(`architecture.md` "Where an `impl` applies").

**Inherited default bodies** ride the same machinery. A trait method's default
body gets a `FunDef` of its own at resolve time
(`resolve_trait_default_impl` → `TraitMethodDef.default_impl`) whose *first
type parameter is `Self`*, bounded by the trait — followed by the trait's own
type parameters, if it has any:

```
trait Show { fun twice(self) -> Int { self.show() + self.show() } }
⇒            fun twice<Self: Show>(self: Self) -> Int { ... }
```

The trait's own `method_type` keeps stating `Self` as the trait type — that is
what impl conformance and call sites are checked against — but a `Subst` is
keyed by *name*, so a `TY_TRAIT` could never be bound by one. Projecting the
signature onto a real `TY_GENERIC` once (`trait_project`) makes the body an
ordinary generic function: `self.show()` inside it dispatches through `Self`'s
bound exactly as it would in a `<T: Show>` function, and one copy is compiled
per receiver type. Every call routed through a trait — a bound, or an impl
that omitted the method — records `Self` in its `ExprMethodCall.inst`
alongside the trait's type arguments and the method's own, which is the whole
key `cg_inst_key` needs. An impl method has no parameter of that name, so on the
calls that land on one the extra binding is simply unused.

`MONO_MAX_DEPTH` (32) bounds the chain. `fun grow<T>(v: T) { grow([v]) }`
type-checks but names a *different* instantiation at every level, each keyed
one array deeper than the last, so nothing converges. The slot space would
stop it eventually — and now that both it and the intern table grow rather
than abort, only eventually — so the limit lives where the divergence is.

### Trait objects

Monomorphisation rests on the observation that *the runtime is uniform in
type arguments*, so a type argument changes exactly one thing: which function
a call resolves to. A trait object is the case where that choice cannot be
made at compile time — so it is **carried by the value**. A `dyn Shape` is
the pair `(value, the table of slots monomorphisation would have picked)`.
That is the whole feature; nothing else about the representation changes.

A `VTable` (`include/object.h`) is one `FunDef *` per method in the trait's
**supertrait closure** — the supers' methods first, then its own, each trait's
in declaration order — already resolved and monomorphised for one concrete self
type. A trait with no supers is the degenerate case of that, its own methods and
nothing else, which is why the layout needed no special case when supertraits
arrived. Filling a slot is the same two-way choice
`compile_method_call` makes for a bound receiver — the impl's own method, or
the trait's default body instantiated at `Self` = that type
(`cg_dyn_slot_target`) — just made ahead of time for every method at once.

Vtables live in `exe->vtables`, appended during codegen exactly as
monomorphised globals are, and for the same reason: which `(trait, type)`
pairs a program needs is a property of its coercion sites. `Mono.vtables`
memoises the compile-time key so two coercions of one type to one trait share
a table; the slot is reserved *before* the table is filled, since compiling a
method body can reach the same pair again (`mono_request` does the same). One
slot per method in the closure, indexed by position (`trait_flat_method_index`,
the same call every dispatch site makes — and always against the trait the
*value* was written as, so build and dispatch cannot disagree) — but a method the trait **excluded
from dispatch** (an undispatchable *provided* method: `Iterator::map`, or
`Iterator::max`, whose `where Self.Item: Ord` asks about the very type the
coercion erased) gets no target: `cg_vtable_for` skips it, leaving the slot
`NULL`. The checker has
already forbidden reaching it through the `dyn`, so the `NULL` is never indexed;
keeping the slot (rather than compacting the table) is what keeps
`OP_DYN_METHOD`'s index the method's plain position.
Note what the runtime `VTable` does **not** hold: the trait and the self type.
Those are compiler bookkeeping the VM never reads — the serialization rule,
one level over. **Nor does it hold the associated-type bindings.** A
`dyn Iterator<Item = Int>` and a `dyn Iterator<Item = String>` are different
types to the checker and the same shape to the VM, because an associated type
is erased exactly as a type argument is — so naming one at a coercion site
changed nothing in codegen, the vtable, the opcodes or the image format. The
memo key stays `(trait reference, self type)`, which still determines the
bindings: an impl binds each associated type once. The *reference* rather than
the trait, since milestone 28: `dyn Into<Int>` and `dyn Into<String>` over one
self type name two impls, so they are two tables — and a trait's type argument
is erased at runtime exactly as an associated type is, so that is again the
only thing that changed.

Five opcodes:

- `OP_MAKE_DYN <vtable>` pops a value and pushes it wrapped as an `ObjDyn`.
  Codegen emits it from `compile_expr`, which wraps every expression so a
  coercion the checker recorded on a node cannot be missed by whichever of
  the ~30 expression cases produced the value.
- `OP_DYN_METHOD <index>` pops the trait object and pushes its method's
  function *then* the unwrapped receiver. That leaves the stack in the shape
  `OP_CALL` already understands — callee beneath its arguments, concrete
  receiver as argument zero — so dynamic dispatch needs no call machinery of
  its own, the same way methods reuse `OP_GET_GLOBAL`.
- `OP_DYN_IS <vtable>` peeks the trait object and pushes whether it carries
  *that* table — the whole of a downcast's runtime test (milestone 77).
- `OP_DYN_INNER` pops a trait object and pushes the value inside it, emitted
  only on the branch an `OP_DYN_IS` just proved.
- `OP_DYN_UPCAST <pair>` pops a `dyn Sub` and pushes the same inner value
  carrying the `dyn Super` table (milestone 78) — see below.

**The vtable is a runtime type identity, and this is where that gets spent.**
The memo above hands out exactly one table per `(trait reference, self type)`
for the whole program, so two trait objects share a table precisely when they
were coerced from the same type. A downcast site knows both halves of that key
statically, so `compile_downcast` asks `cg_vtable_for` for the table `T` *would*
have been coerced through and the test is one pointer comparison. Nothing new
had to be carried: no type tag on the value, no reflection table, and nothing
in the image format — the operand is a vtable slot, which `OP_MAKE_DYN` already
serialized, and `bc_read` allocates one `VTable` per slot so pointer identity
survives the round trip. A downcast site is simply another vtable *requester*,
so a program that only ever asks whether something is a `Point` still slots
that table.

Two things fall out of the key being the type rather than the value's shape.
`Box<Int>` and `Box<String>` share one `StructDef` — structs are not
monomorphised — yet they are distinct interned types and so two tables, which
is why erasure never bites. And since the key is the trait *reference*, one
self type behind `dyn Sink<Int>` and `dyn Sink<String>` has two tables; a
downcast reads its half of the key off the operand, so it always asks about the
reference the value was written as.

#### Upcasting: the link lives on the table, not at the site

The downcast recognises a table; the **upcast** (milestone 78) cannot, because
the two closures are laid out independently. With `trait Both: Left + Right`,
`Both`'s table is `[Base, Left, Right, Both]` and `Right`'s own is
`[Base, Right]` — so `right` is at index 2 in one and 1 in the other, and
reusing the sub's table as a prefix of the super's would call `left` and say
nothing (`tests/run/dyn_upcast_diamond.dt` pins exactly this). A `dyn Sub` -> a
`dyn Super` therefore keeps its inner value and **swaps tables**.

The difficulty is that the two ends of the operation know different halves of
the key. An upcast *site* knows both traits and not the concrete type; the table
the value carries knows the concrete type and only its own trait. So the link is
stored where both are known — on the source table:

```c
struct VTable { FunDef **methods; int method_count;
                VTableUpcast *upcasts; int upcast_count; };   // {u16 pair, VTable *to}
```

`pair` is a dense compile-time numbering of the `(source trait reference, target
trait reference)` spellings the program contains (`Mono.upcasts`), which is all
a site can name; the VM only compares it. Computing every link is a fixpoint,
and `cg_upcast_pair` / `cg_link_upcast` run it incrementally in two halves: a
newly recorded pair links every table of that source trait that already exists,
and `cg_vtable_for` links every already-recorded pair when it builds a new one.
Doing it incrementally rather than as a pass after the drain matters, because
building a target table queues bodies — a post-pass would have to restart the
worklist.

**So the upcast does build a second table, but at compile time.** That is
possible only because the set of concrete types that can be behind a `dyn Sub`
is not open: every one of them was coerced somewhere, and `Mono.vtables`
filtered by trait *is* that set. At run time `OP_DYN_UPCAST` is a scan of a list
that is almost always one entry long, plus the `ObjDyn` the new pairing needs.

The image carries the links (`BC_VERSION 5`) as one `[u16 count, (u16 pair,
u32 vtable)*]` record per vtable, written after every table so a link may point
forward. It survives the serialization rule for the same reason the downcast
did: a link is two tables and an opaque id, and none of those is a type.

The receiver is unwrapped because the method was compiled for the concrete
type, not for the trait object; coercion *wraps*, it never converts, so the
inner value is byte-identical to what was coerced. GC marks only `inner` —
the vtable is arena-allocated with the `Executable`, and its methods' constant
pools are already roots by virtue of being in `exe->globals`.

`value_print` and `value_equal` both see through the wrapper, so a `dyn Shape`
over a `Sq` prints and compares as the `Sq`.

### Propagate (`?`)

`Ok`/`Err` are both single-field tuple variants of the *same* enum as the
`?` operand and the enclosing function's return type (enforced by the
checker, which caches both `VariantDef`s on the expr node) — and since
generics are erased at runtime, an `Err(e)` instance is already
correctly-shaped for the return position regardless of whether the operand's
and the function's `Ok` payload types differ. So `expr?` never reconstructs
anything: duplicate the operand, read its tag (`OP_TAG`), and either
`OP_FIELD_GET 0` the `Ok` payload (the expression's value) or `OP_RETURN`
the whole instance unchanged (propagating `Err` up to the caller) — the same
"just emit `OP_RETURN` wherever, `vm_run` unwinds the frame regardless of
what else is logically live" trick `return` statements already use.

### Closures & upvalues

A closure expression (`|x| body`) is compiled the crafting-interpreters
way: its body goes into its own `Chunk` via a *child* `Cg` whose `parent`
points at the enclosing function's `Cg`, and the enclosing chunk emits
`OP_CLOSURE` to build the runtime `ObjClosure` from the captured cells.

The child `Cg` inherits `subst` and `impls` from its parent — a closure body
sees the enclosing body's type parameters and selects impls from the same set
— which makes a closure inside a *generic* body **a different program per
instantiation**, since `v.show()` inside it resolves through those type
arguments exactly as it does outside. So it gets its own `FunDef` clone
whenever `cg->subst` is non-empty, for the same reason and in the same shape as
`mono_request` cloning the body around it. `ExprClosure.def` is the AST node's,
one for every instantiation; writing a chunk straight into it lets the last
instantiation compiled win for all of them, including the earlier chunks whose
`VAL_FUN` constant already names it. A non-generic body has an empty `subst`
and is compiled once, so it keeps the AST's def and pays nothing.

**Resolving a name** (in `EXPR_PATH` and assignment targets) now tries, in
order: a local of this function (`OP_GET_LOCAL`), an *upvalue*
(`cg_resolve_upvalue` → `OP_GET_UPVALUE`), then a module global
(`OP_GET_GLOBAL`). `cg_resolve_upvalue` walks the `parent` chain: if the name
is a local of the enclosing function it records `{is_local=true, slot}` and
marks that local `is_captured`; otherwise it resolves the name as an upvalue
of the *parent* (recursively) and records `{is_local=false, parent_upvalue}`.
So a multi-level capture threads one cell down through every intervening
closure's upvalue array. `cg_add_upvalue` de-duplicates, so two references to
the same captured variable share one runtime cell. Assignment to a captured
variable is symmetric (`OP_SET_UPVALUE`), which is what makes a mutable shared
cell (two closures over one `var`) behave.

`self` is one of those names: `compile_fun_body` registers the receiver as a
local *called* `"self"` (the parser leaves the parameter nameless, and `self`
is a keyword, so nothing else can claim the slot), so a closure inside a
method captures it like any other local — `EXPR_SELF` reads `Cg.self_slot`
when it is this frame's, and falls back to `cg_resolve_upvalue` when it is an
enclosing one's.

**Upvalues are open then closed.** While the defining frame is alive an
`ObjUpvalue`'s `location` points straight at the variable's stack slot — reads
and writes go through the live slot, so the variable and every closure over it
see the same value. When that slot is about to die the upvalue is *closed*:
the value is copied into the upvalue's own `closed` field and `location` is
repointed there, so the closure keeps working after the frame is gone. The VM
keeps one `open_upvalues` list (ordered by descending stack address);
`capture_upvalue` reuses an existing entry for a slot (that's the sharing) or
inserts a new one, and `close_upvalues(last)` closes everything at/above
`last`.

Two things drive closing:
- `OP_RETURN` closes from `frame->base` — covers function params and
  function-level locals a returned/escaping closure captured (the common
  case), including early `return`s and `?`.
- `OP_CLOSE_UPVALUE slot`, emitted by `cg_close_scope` at every point a scope
  pops locals (block exit, loop exit, `break`/`continue`, match-arm cleanup)
  *when* one of the popped locals was captured — covers a capture inside a
  block/arm that escapes to an outer binding while the function keeps running.
  Block-declared captures are therefore per-iteration in a loop; the loop
  *variable* itself is one shared cell closed at loop exit (a captured `for`
  variable reads its final value — a deliberate, documented simplification,
  not UB).

Non-capturing functions (top-level, methods, and closures that happen to
capture nothing) stay plain `VAL_FUN`; only capturing closures become
`ObjClosure`. `OP_CALL` accepts either — a `VAL_FUN` frame has `closure ==
NULL` (it never executes an upvalue op), an `ObjClosure` frame carries the
closure so `OP_GET/SET_UPVALUE` can reach its cells.

## VM (`src/vm.c`)

Fixed arrays: 4096-value stack, 128 call frames. A frame is
`{FunDef*, ObjClosure* (NULL for a plain-function call), ip, base}` with
`base` pointing at arg slot 0; the callee value lives at `base - 1`. The VM
also threads one `open_upvalues` list of live captures over stack slots (see
"Closures & upvalues"). Runtime errors (division by zero, stack overflow,
calling a non-function, array index out of bounds) print
`runtime error: ...` plus a call trace of function names and make the
process exit 1. The checker guarantees operand kinds, so opcode handlers
don't re-validate types (bounds are the one runtime-only check, since array
lengths aren't static).

## Linking

A program is every module reachable from the root file, and a chunk compiled
from one module routinely names a definition from another — an imported
function, a struct whose constructor it calls. But a slot operand is a fixed
width with no module field, so it cannot mean "index into my own module":
`OP_GET_GLOBAL 3` has to identify one function in the whole program.

That flat namespace lives in an `Executable` (`include/object.h`):

| Table | Contents | Operand of |
|---|---|---|
| `globals[]` | every reached top-level fun, impl method, native and monomorphised instance | `OP_GET_GLOBAL` |
| `structs[]` | every struct something constructs | `OP_STRUCT` |
| `enums[]` | every enum something constructs a variant of (variant tags stay per enum) | `OP_ENUM` |
| `vtables[]` | one per (trait, concrete type) pair coerced | `OP_MAKE_DYN` |
| `closures[]` | nested closure `FunDef`s, appended during codegen | nothing — GC roots only |

Each assigned index is written back into the definition's `slot`, so codegen
only ever emits `def->slot` and never asks which module a definition came
from. More than `SLOT_MAX` (65536) of any of them exceeds the operand width
and is reported against the program rather than against any one declaration.

### Reachability

Every table is filled **on demand**, and each grows (doubling from 16) as the
walk reaches definitions. Pre-sizing to the operand space is what they used to
do, back when that space was 256 entries; at 65536 it would be a wasted 64K
pointers apiece for a ceiling almost nothing comes near.

That leaves **nothing for a linking step to do**. All that survives of it is
`exe_assign_tags` (`src/codegen.c`), which fixes the variant tags — the one
thing here that is not a slot: a tag is an index within its own enum, fixed by
declaration order, and a `match` reads it off an enum the program may never
construct, so it cannot wait for a reference that may not come. Slots became
demand-driven in milestone 21 and the tables learned to grow in milestone 22;
with both gone there is no longer a linking pass distinct from the walk.

Slots are then handed out by codegen, as it discovers references:

- `compiler_codegen` finds `main`, calls `mono_seed` to give it slot 0 and
  queue its body, and drains the worklist. Compiling a body slots and queues
  whatever it names — its calls, the vtables its coercions build, the generics
  it instantiates — so this is a worklist, not a pass over the registry, and a
  module no reachable body names is never visited at all.
- `cg_call_target` is the single funnel for a function reference (`OP_GET_GLOBAL`
  is emitted in exactly one place). `mono_reach` handles the non-generic case:
  no copy, an empty key, and `slot != SLOT_NONE` is the memo — which is what
  makes a recursive call terminate, exactly as `mono_request`'s memo does.
- `cg_reach_struct` / `cg_reach_enum` do the same for the other two spaces, at
  the constructor. Neither is reached by being declared, nor by being matched:
  a pattern tests a tag and reads fields by index, and neither operand names
  the definition.

**A non-generic body uses its own module's impl set, not the requester's.** A
bound's witness travels with a type argument because the argument is chosen at
the call site (see "Monomorphisation"); a non-generic body has no bound to
witness and selects impls entirely from where it was written — the same set
the checker used on it. So who reaches it first cannot change what it compiles
to.

The consequence is that **slot pressure is a property of what a program uses,
not of what its imports declare**. `use std::result::Result;` no longer spends
globals on the combinators the program never calls: `tests/run/std_result.dt`
went from 24 globals to 16, `std_option.dt` from 24 to 17. A program that
imports everything and uses everything is unchanged (`std_cmp.dt`: 16 either
way). `tests/run/unused_defs/` declares 260 functions and links because it
calls three.

The price is stated plainly in `tests/run/unreachable_body.dt`: **a body
nothing reaches is never handed to codegen**, so a construct the VM refuses
inside one goes unreported. This was already true of every generic definition
— an uncalled one has no body to compile — and is now true uniformly. Only
codegen is demand-driven; the checker still sees every function in every
module, so a compile-only run (`./build/ducktape file.dt`) is unaffected.

Two ordering constraints survive, and both are why linking is still a separate
step rather than something the first body does on its way past:

- **The tables exist before any chunk.** Compiling module A can emit a slot
  for a definition in module B, so the space they share must already be
  allocated — a compile-as-you-go scheme would need a patch-up pass over
  emitted bytecode.
- **The heap roots off those tables.** Codegen interns string literals, which
  can trigger a collection while most chunks are still empty. A `FunDef` in
  the tables with a NULL chunk is a root with nothing to mark; one missing
  from the tables is not a root at all, and its already-interned constants
  would be swept. This is why a slot is handed out *before* the body that
  earns it is compiled.

Name resolution stays module-local: a bare `foo()` is compiled from
`ExprPath.resolved_fun`, the `FunDef` the *checker* picked, because the name
may be an alias (`use lib::helper as h;`) or belong to another module
entirely — a search over the enclosing module's own `funs[]` would find
neither.

Name resolution stays module-local: a bare `foo()` is compiled from
`ExprPath.resolved_fun`, the `FunDef` the *checker* picked, because the name
may be an alias (`use lib::helper as h;`) or belong to another module
entirely — a search over the enclosing module's own `funs[]` would find
neither.

## Serialization (`src/bytecode.c`, `include/bytecode.h`)

`--emit-bc <out> file.dt` writes the linked program to a flat binary image;
`--run <image>` decodes it and executes it with no compiler in the process.
`--run` picks the path by sniffing the four magic bytes rather than the file
extension.

```
header   "DTBC", u16 version, u32 entry (a globals index)
counts   u32 strings, globals, structs, enums, closures, vtables
strings  [u32 len, len bytes]*        — every name and string constant
structs  [u32 name, u8 is_tuple, u16 field_count, u32 field_name*]
enums    [u32 name, u16 variant_count,
            [u32 name, u8 is_tuple, u16 field_count, u32 field_name*]*]
vtables  [u16 method_count, u32 fun_index*]*   (0xFFFFFFFF == excluded slot)
funs     [u32 name, u16 param_count, u8 has_chunk,
            u32 code_len, code_len bytes,
            u32 const_count, const*]*  — globals first, then closures
```

Every multi-byte field is little-endian and written byte by byte, so an image
depends on neither the host's byte order nor any struct layout. Floats go out
as their IEEE-754 bit pattern.

**An image is the runtime projection of the program.** Only what `vm.c` and
`value_print` actually read off a `FunDef`/`StructDef`/`EnumDef` is written —
names, arities, field shapes, chunks. Types, spans and the owning `Module` are
compile-time concerns, so a loaded def has them NULL: an image has no AST
behind it, and nothing in the VM asks for one.

`BC_C_CHAR` is the first constant kind added since the format was written
(milestone 9a), and the only one whose payload is *not* total over its bits: a
u32 is a scalar value only if it is in range and not a surrogate, so the loader
checks it rather than trusting it. Every other tag can decode anything it
reads. Appending the tag rather than inserting it keeps the existing numbering,
which costs nothing and means an older image's bytes still mean what they said.

**The pool holds no raw pointers.** A string constant becomes a string-table
index, a `VAL_FUN` constant an index into the funs section (globals, then
closures — a nested closure function is reachable only through a constant, so
it has no slot of its own). That is what keeps both directions a straight
loop. Struct and enum references need no encoding at all: `OP_STRUCT`/`OP_ENUM`
already carry a slot the VM resolves through `exe`, and the tables are written
in slot order. Variant tags are positional, exactly as `exe_assign_tags` assigns them.
A vtable is likewise written as bare function indices: which trait and which
self type it was built for is compile-time bookkeeping, so it is not in the
image at all. An **excluded** slot (a `NULL` where the trait left an
undispatchable provided method out of dispatch) has no function to index, so it
goes out as the sentinel `0xFFFFFFFF` — out of range for any real globals index
— and decodes back to `NULL`. Vtables sit between the data definitions and the code — they
only point *into* globals, which already exist, and being decoded before any
chunk means an `OP_MAKE_DYN` operand is valid as soon as code carrying it can
run.

Two ordering constraints, both inherited from linking:

- **The string table precedes everything that indexes it,** so writing is two
  walks — one that only interns, one that only writes. The writer checks the
  table didn't grow during the second walk; if it did, the two walks disagree
  about which strings an image mentions.
- **Every definition is allocated before any record is decoded.** That is what
  lets a `VAL_FUN` constant name a function further down the file without a
  patch-up pass, and it is why `bc_load` — not its caller — calls `heap_init`:
  decoding a chunk interns its string constants, so the heap must already be
  rooted off complete tables (chunks still NULL) before the first pool is read.
  Same shape as the codegen constraint above, one level over.

Every read is bounds-checked against the buffer, and section counts are
sanity-checked against `SLOT_MAX`, so a truncated or corrupt image is rejected
rather than executed. The version byte covers the operand widths: widening a
slot operand changes how existing code bytes decode, so an old image must be
refused rather than misread. `tests/run` doubles as the round-trip
suite: `scripts/run_tests.sh` emits an image for every runnable program and
re-runs it, so a construct the format forgets shows up as an output diff.

## Native functions (`src/native.c`, `include/native.h`)

A function whose body is in C, declared bodyless in an ordinary std module
with an attribute naming it (`language.md` "Native functions"). The mechanism
is one field on `FunDef` and one branch in `OP_CALL`.

**Registry.** `src/native.c` is two tables mapping a name to either a C
function (`@native`) or an `OpCode` (`@intrinsic`). Nothing in it knows about
types: the signature lives in the `.dt` file and is the checker's business, so
a native's contract with C is only "this many values in, one value out".
`tc_bind_native` does the lookup during registration — in the checker rather
than in codegen, because that is where the attribute still has a span to
report an unknown name against, which a hand-built builtin never had.

**`@native` is an ordinary global.** It takes a slot in `exe->globals` like
any other definition, so `OP_GET_GLOBAL` reaches it and it is first-class for
free: passable, storable in an array, capturable by a closure, usable as a
vtable entry. `OP_CALL` opens no frame for it — it runs the C function in
place and pushes the result:

```
if (fun->native != NULL) {
    NativeCtx ctx = { .heap = vm.heap, .error = NULL };
    Value result = fun->native(&ctx, vm.sp - argc, argc);   // args stay put
    ...
    vm.sp -= argc + 1;                                      // args + callee
    push(&vm, result);
}
```

The **calling convention is a GC rule**: the arguments stay on the value stack
across the call and are only popped once the result exists. The stack is the
root set, so an allocating native (`string_slice`, `fmt_float` and
`strbuf_push_char` intern or grow a buffer) cannot have its own arguments swept
out from under it. `OP_MAKE_DYN` already followed exactly this
discipline for `heap_dyn`; getting it wrong reproduces the 5c-ii class of bug,
something reachable-looking the collector cannot see. A native fails by
setting `ctx->error`, which becomes a runtime error at the call site.

### Calling ducktape from a native

A native that takes a *function value* — a comparator, a predicate — cannot
answer out of C alone: the decision belongs to the program. `native_call` is
that direction of the boundary, and `std::sort`'s `sort_by` is its first
caller.

```
bool native_call(NativeCtx *ctx, Value callee, const Value *args, int argc,
                 Value *out);
void native_root(NativeCtx *ctx, Value v);   // keep alive across a call
void native_unroot(NativeCtx *ctx, int count);
```

**The interpreter became re-entrant, which is the whole of the mechanism.** The
bytecode loop is now `run(Vm *vm, int stop_depth)`, and `OP_RETURN` ends it when
`frame_count` falls back to `stop_depth` rather than to zero — so a caller in C
can push a frame and drive exactly that call to its return, leaving the outer
`run` suspended in C with its frames untouched beneath. `vm_run` is `run(&vm, 0)`
and is the loop it always was. The call itself is built exactly as `OP_CALL`
builds one (callee, arguments above it, a frame over them, result left where the
arguments were), which is why a comparator may equally be a closure or a plain
`VAL_FUN` passed by name.

Three consequences, and they are the reason this is a boundary rather than a
convenience:

- **A collection can now happen inside a native.** The callee is ordinary
  ducktape and allocates. Anything the native holds must therefore be a root:
  its arguments already are, and `native_root` puts anything else on the stack
  above them (a push, since the stack *is* the root set). Every root must be
  dropped before returning — `OP_CALL` asserts the stack came back level,
  because a native that returned askew would corrupt the frame beneath it
  instead of failing.
- **Any pointer *into* a heap object may be stale after a call.** The callee can
  push to the very array the native is walking, and `heap_array_reserve`
  reallocates `items`. `sort_by`'s answer is to work on scratch the program
  cannot reach, and to touch the caller's array only before the first comparison
  and after the last.
- **A failing callback is already reported.** `run` printed the error with the
  frames that were live when it happened, so `native_call` sets
  `ctx->unwinding` and the VM unwinds without describing the same failure a
  second time from the outside. A native seeing `false` must return promptly,
  having first dropped its roots.

`tests/run/sort_callback.dt` exercises all of it — an allocating comparator, a
named function rather than a closure, a comparator that sorts (the interpreter
re-entered twice over) — and under `--gc-stress` it is the rooting test:
deleting the `native_root` calls turns it into a heap corruption abort rather
than a wrong answer.

**Panic is that failure path, named.** `std::panic`'s `panic_abort` sets
`ctx->error` and nothing else, so a ducktape-level panic and a failing native
are the same event — `runtime_error` prints the message and the frames beneath
it, and `vm_run` returns false. There is no unwinding: no `catch`, no
destructor to run, no value carried out. What makes it usable from ducktape is
purely a *checker* fact, that the declared return type `Never` stands in for
any type (`language.md` "`std::panic`"); the VM needed no change at all.

The message is the argument's own bytes rather than a copy — an `ObjString` is
NUL-terminated, and `args` is still on the stack when the VM reads
`ctx->error`, so nothing can collect in between. Aliasing the heap that way is
only sound *because* a panic never returns; a native that wanted to fail with a
computed message and keep running would need somewhere else to put it.

**The character walk's natives have nothing to say about that rule, which is
what makes them cheap.** `string_char_at` answers a `Char` and
`string_prev_boundary` an `Int`; both are values, so nothing allocates, nothing
can collect, and there is no rooting question at all — the walk's state lives
in a ducktape struct (`std::iter`'s `CharIter`) rather than in C. Until
milestone 61 the crossing was `string_chars`, a two-pass native that counted
and validated, then allocated an array of exactly that size and filled it; the
two passes were that rule, since growing the array as it went would have kept a
partially-filled array live across each growth. What survives of it is
`string_char_count` — the counting pass alone, kept because `std::string`'s
`pad_*` lang items need a character count and cannot import `std::iter` — and
the array it used to build is now `s.chars().collect()`, grown by the ordinary
`array_push` under the ordinary rule.

**One native reads bytes the decoder did not hand it**: `string_prev_boundary`
scans backwards over continuation bytes (`(b & 0xC0) == 0x80`) to find where the
character ending at an offset begins. The scan is bounded by four rather than
open, since that is the longest a UTF-8 sequence gets, and the lead byte it
lands on is then decoded and required to span exactly to the offset it started
from — so malformed bytes fail here the same way they fail going forwards,
instead of being walked off the front of the string.

**A generic native is never monomorphised.** The runtime is uniform in type
arguments, so one C body serves every `T` — `cg_call_target` returns the
`FunDef` itself rather than keying an instance, and `exe_slot_fun` gives it a
slot despite `fun_is_generic`. `print<T>` is the whole of that case, and it is
the monomorphisation observation paying out once more.

**`@intrinsic` is not addressable.** It *is* an opcode, so it gets no slot;
`compile_call` recognises the callee's `FunDef` (from `ExprPath.resolved_fun`,
so an alias works) and emits the opcode inline with no callee value and no
frame. Anything that would need a value — naming it, passing it — reaches
`cg_call_target` and is a diagnostic. This is what finally exposes `OP_LEN`,
until now reachable only from the `for` desugaring — and, in milestone 60,
`OP_RANGE_START`/`OP_RANGE_STOP` the same way, which is what let `std::iter`'s
`RangeIter` be written in ducktape instead of built in. The tier's constraint
is what decides which opcodes can go in the table: no operand bytes, popping
exactly the declared parameters and pushing exactly the declared return, since
the opcode is emitted bare with nothing to encode an operand into.

**A method may be native too.** `resolve_impl_decl` runs the same
`tc_bind_native` on an impl method's `FunDef` that `tc_register_fun` runs on a
top-level one, so `impl String { @native("string_len") fun len(self) -> Int; }`
binds exactly as a free native declaration does — which, since milestone 40, is
how `std::string` spells `len`. `self` is an ordinary parameter
(`is_self` set), and the receiver's value is what the free-function form would
have passed as argument 0 — so a native method needs *no* codegen change at all:
`compile_method_call` already pushes the arguments in `is_self` order and closes
with `OP_CALL`, which dispatches `fun->native`. That is why a native method is
first-class in the same breath — it takes a global slot and drops into a
`dyn Trait` vtable like the free form. An **intrinsic** method is the one place
`compile_method_call` grew a branch, mirroring `compile_call`: it pushes the
receiver at its `self` position and emits the opcode inline, so the
`cg_call_target` "an intrinsic has no slot" diagnostic is never reached for a
method. (A *trait*-declaration method still cannot be native: its default body is
generic over `Self`, with no concrete C body to bind.)

**Serialization.** A C function pointer cannot go in an image, so a fun record
carries a *body kind*: `BC_BODY_CHUNK` writes the chunk as before,
`BC_BODY_NATIVE` writes the registry name, and `bc_load` re-binds it against
the running binary. A name this build does not know is an error at load rather
than a wrong call later, which is why the image needs no separate registry
hash. An `@intrinsic` never appears in an image at all — its opcode is already
in the emitted code.

**`hash_mix` / `hash_string`** (milestone 63) were the tier's clearest case
while what put them here was *expressibility* rather than cost: the language had
no bitwise operator at all, so a mixing function was not slow to write in
ducktape, it could not be written. **Milestone 65 closed that**, and
`tests/run/bitwise_mixer.dt` now writes `hash_mix` out in ducktape constant for
constant and checks the two agree. They stay in C as an optimisation: `mix` is
six operations of pure arithmetic with no callback, the shape the cost model
below says to move. Keeping the ducktape twin under test is what stops that
from being a black box. `hash_mix` folds one Int into a running state: a splitmix64 finalisation of the incoming value first, so that
the low-entropy keys a program actually uses (0, 1, 2) still spread across all
64 bits, then FNV-1a's prime to carry the accumulation and a last shift-xor to
move the high bits down where `%` will read them. Every multiply relies on
wrapping, done in `uint64_t` so it is defined rather than merely what the
hardware does; the `Int` the language hands back wraps to match.

`hash_string` allocates nothing and computes nothing: it returns
`ObjString.hash`, which `heap_intern` already filled in to find the string's
bucket. So the one type whose hash would otherwise be a walk over its bytes is
the one type that costs a field read — and because interning makes two equal
Strings one pointer, the value is consistent with `==` for exactly the reason
the intern table already depends on. Both take Ints and Strings and return an
Int, so neither touches the heap or the collector, and `std::collections`'s
whole open-addressed table is ordinary ducktape written on top of them.

Porting `print` to this deleted `OP_PRINT`, `cg_names_builtin_print`,
`tc_register_builtins`, the "using a builtin as a value" diagnostic, and the
`std::io` no-op in `mod_collect_imports`/`tc_link_imports`. `print` is now
imported like anything else, and is deliberately kept out of the prelude
(milestone 45) — that prelude covers the lang-item and vocabulary modules, not
plain functions like `print`.


## Future (design intent, not implemented)

- **REPL:** does not exist; would re-run the pipeline per line with a fresh
  arena, keeping the GC heap alive across lines.
