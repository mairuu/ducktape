# ducktape — runtime (codegen + VM)

`./build/ducktape --run file.dt` compiles the checked AST to bytecode and
executes `main()`. Entry: `compiler_execute` (`src/compiler.c`) →
`codegen_module` (`src/codegen.c`) → `vm_run` (`src/vm.c`). `--gc-stress`
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
| `VAL_UNIT` | — |
| `VAL_RANGE` | start, end (`int64_t`), inclusive flag |
| `VAL_FUN` | `FunDef *` (top-level function, no captures) |
| `VAL_OBJ` | `Obj *` — string or array, see "Heap & GC" |

`value_equal` (`src/value.c`) is the one equality used by `OP_EQ`/`OP_NEQ`:
strings compare by pointer (interning makes that correct), arrays compare
structurally (elementwise, recursive).

## Bytecode (`include/chunk.h`)

A `Chunk` per function (`FunDef->chunk`, arena-allocated): growable code
bytes + constant pool (≤256 consts, u8 index). Operands: u8 unless noted.

| Ops | Notes |
|---|---|
| `OP_CONST` `OP_UNIT` `OP_TRUE` `OP_FALSE` | push |
| `OP_POP` `OP_POPN n` `OP_SLIDE n` | SLIDE drops n values *beneath* the top — how a block discards its locals while keeping its tail value |
| `OP_GET_LOCAL` `OP_SET_LOCAL` | frame-relative slot; SET peeks (assignment is an expression) |
| `OP_GET_GLOBAL` | pushes `VAL_FUN` of `module->funs[slot]` |
| `OP_ADD/SUB/MUL/DIV/MOD` | int×int stays int; any float widens (matches checker); div/mod by zero int → runtime error; float mod = `fmod` |
| `OP_NEG` `OP_NOT` `OP_EQ/NEQ/LT/LTEQ/GT/GTEQ` | comparisons widen like arithmetic |
| `OP_CAST_INT` `OP_CAST_FLOAT` | dynamic: no-op if already the target kind |
| `OP_RANGE incl` `OP_RANGE_START` `OP_RANGE_TEST` | build range from end/start; TEST pops i+range, pushes `i < end` (or `<=`) |
| `OP_JUMP u16` `OP_JUMP_IF_FALSE u16` `OP_LOOP u16` | JUMP_IF_FALSE does *not* pop the condition |
| `OP_CALL argc` | callee value sits beneath the args |
| `OP_PRINT` | pops, prints + newline, pushes unit |
| `OP_RETURN` | pops result, tears down the frame (incl. callee slot), pushes result for the caller |
| `OP_ARRAY count` | pops `count` elems (left-to-right order), pushes a new array |
| `OP_INDEX_GET` | pops index, array; pushes `array[index]` (bounds-checked) |
| `OP_INDEX_SET` | pops value, index, array; sets `array[index] = value`, pushes value back (bounds-checked) |
| `OP_LEN` | pops array; pushes its length as `Int` |
| `OP_INTERP seg_count` | pops that many values, stringifies + concatenates them in order, pushes the result string |
| `OP_DUP depth` | pushes a copy of the value `depth` slots below the top |

`OP_ADD` additionally handles `String + String` (interned concat) alongside
its numeric cases; the checker guarantees operand kinds so no other opcode
needs a string case.

## Heap & GC (`include/object.h`, `src/object.c`)

`Heap` (one instance, created in `compiler_execute`, threaded through both
`codegen_module` and `vm_run`) owns every runtime object. Every object starts
with an intrusive `Obj` header (`kind`, `marked`, `next` — the all-objects
list used by sweep):

| `ObjKind` | Payload |
|---|---|
| `OBJ_STRING` | interned: `len`, cached `hash`, flexible `chars[]` (NUL-terminated) |
| `OBJ_ARRAY` | `count`, growable `Value *items` |

String identity is pointer identity — `heap_intern` looks up an
open-addressing table (FNV-1a hash, linear probing, tombstone deletes)
before allocating, so `==` on strings is a pointer compare. Codegen interns
string *literals* at compile time (`cg_decode_string` in `src/codegen.c`,
decoding the scanner's `\n \t \r \" \\ \{` escapes); the VM interns at
runtime for `+` concat (`heap_concat`) and interpolation (`OP_INTERP`,
`stringify` in `src/vm.c`).

**Collection** is mark-sweep, triggered from `heap_alloc` (the sole entry
point that grows `bytes_allocated`) once it crosses `next_gc` (starts at
1 MiB, doubles `bytes_allocated` after each cycle), or unconditionally under
`--gc-stress`. Roots: every compiled function's chunk constant pool (scanned
directly off `Heap.module`, so constants need no special "immortal" case)
plus the VM's live value stack, reached through `Heap.mark_roots` — a
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

Any allocating call (`heap_intern`, `heap_concat`, `heap_array`) may collect
*before* the new object exists, so the discipline throughout codegen/VM is:
keep every already-live operand reachable from a root (still on the VM
stack, not yet popped) until the new object is built and pushed. See the
`OP_ARRAY`/`OP_INTERP` handlers in `src/vm.c` for the pattern (index math
into the stack in place, decrementing `sp` only after the result exists).

## Codegen shapes (`src/codegen.c`)

- Locals are compile-time tracked (`Cg.locals`, name → frame slot); the
  initializer's stack slot *is* the variable. Params occupy slots 0..n-1.
- Blocks: compile stmts, compile tail (or `OP_UNIT`), then `OP_SLIDE n`.
- `and`/`or` compile to jump-based short-circuit; `if` always produces a
  value (`OP_UNIT` for a missing else).
- Loops evaluate to unit. `for x in <range>` materializes two locals —
  hidden `range` and the loop variable — then tests with `OP_RANGE_TEST` and
  increments by constant 1. `for x in <array>` (`compile_for_array`)
  dispatches on `iterable->resolved_type->kind`; it materializes three
  locals — hidden `arr`, hidden `idx` (starts at 0), and the loop variable
  (starts as unit) — tests `idx < OP_LEN(arr)`, reads `arr[idx]` into the
  loop variable at the top of each iteration, and increments `idx` at the
  continue target. `break`/`continue` emit `OP_POPN` down to the loop's
  recorded local base (continue keeps the hidden iter locals, break does
  not) before jumping; forward continue targets the increment.
- Calls: push callee (`OP_GET_GLOBAL` or a local), args left-to-right,
  `OP_CALL`. A call to the unshadowed name `print` lowers to `OP_PRINT`.
- Strings: literals intern directly to a constant (`cg_decode_string`);
  `EXPR_INTERPOLATED` pushes each segment (text segments as string
  constants, expr segments compiled normally) then one `OP_INTERP`.
- Arrays: `EXPR_ARRAY` compiles elements then `OP_ARRAY`; `EXPR_INDEX` is
  object-then-index-then-`OP_INDEX_GET`. Assigning to `arr[i]` (plain `=`)
  pushes array, index, value, then `OP_INDEX_SET`; a compound `+=` etc.
  additionally duplicates array+index with two `OP_DUP 1`s to read the
  current value before combining (`compile_index_assign`).
- Anything outside the subset (structs, enums, match, closures, methods,
  `?`, tuples, generic functions, destructuring) emits a source-anchored
  diagnostic `"... is not supported by the VM yet"` and fails codegen — it
  never crashes at runtime.

## VM (`src/vm.c`)

Fixed arrays: 4096-value stack, 128 call frames. A frame is
`{FunDef*, ip, base}` with `base` pointing at arg slot 0; the callee value
lives at `base - 1`. Runtime errors (division by zero, stack overflow,
calling a non-function, array index out of bounds) print
`runtime error: ...` plus a call trace of function names and make the
process exit 1. The checker guarantees operand kinds, so opcode handlers
don't re-validate types (bounds are the one runtime-only check, since array
lengths aren't static).

## Future (design intent, not implemented)

- **Aggregate runtime (5c):** structs/enums as heap objects, field access,
  methods (`self` dispatch), match compilation (tag dispatch +
  destructuring), closures with upvalues (`is_captured` groundwork exists),
  `?` lowering, tuples.
- **Bytecode serialization** (after a module system exists): flat binary —
  magic/version, string table, recursive chunk records (name, arity, code,
  tagged constants, nested functions). Keep the constant pool free of raw
  pointers (string-table indices instead) so this stays a straight loop.
  Deliberately deferred: the chunk format is still changing.
- **REPL:** does not exist; would re-run the pipeline per line with a fresh
  arena, keeping the GC heap alive across lines.
