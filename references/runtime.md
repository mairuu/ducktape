# ducktape — runtime (codegen + VM)

`./build/ducktape --run file.dt` compiles the checked AST to bytecode and
executes `main()`. Entry: `compiler_execute` (`src/compiler.c`) → `exe_link`
then `codegen_module` per module (`src/codegen.c`) → `vm_run` (`src/vm.c`). `--gc-stress`
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
| `VAL_FUN` | `FunDef *` (top-level function or method — a callable with no captures) |
| `VAL_OBJ` | `Obj *` — string, array, tuple, struct, enum instance, or closure, see "Heap & GC" |

`value_equal` (`src/value.c`) is the one equality used by `OP_EQ`/`OP_NEQ`:
strings compare by pointer (interning makes that correct); arrays, tuples,
structs, and enum instances compare structurally (elementwise, recursive —
enum instances additionally compare unequal if their variant differs).
`value_print` (used by `print` and, indirectly, nowhere else — string
interpolation only accepts Int/Float/Bool/String, see `language.md`) prints
tuples as `(a, b)`, structs as `Name { field: v, ... }` or `Name(a, b)` for
tuple structs, and enum instances the same way but with the variant's name
and no enum qualifier (`Some(1)`, not `Option::Some(1)`), matching how Rust's
`Debug` prints enums.

## Bytecode (`include/chunk.h`)

A `Chunk` per function (`FunDef->chunk`, arena-allocated): growable code
bytes + constant pool (≤256 consts, u8 index). Operands: u8 unless noted.

| Ops | Notes |
|---|---|
| `OP_CONST` `OP_UNIT` `OP_TRUE` `OP_FALSE` | push |
| `OP_POP` `OP_POPN n` `OP_SLIDE n` | SLIDE drops n values *beneath* the top — how a block discards its locals while keeping its tail value |
| `OP_GET_LOCAL` `OP_SET_LOCAL` | frame-relative slot; SET peeks (assignment is an expression) |
| `OP_GET_GLOBAL` | pushes `VAL_FUN` — one program-wide slot space over every module's top-level funs and impl methods: `exe->globals[slot]` (see "Linking") |
| `OP_CLOSURE const, n` `, then n×(is_local, idx)` | reads the closure's `FunDef` from constant `const` (a `VAL_FUN`), builds an `ObjClosure` capturing `n` upvalues, pushes it — see "Closures & upvalues" |
| `OP_GET_UPVALUE` `OP_SET_UPVALUE` | index into the running closure's upvalue array, reading/writing through the (open or closed) cell; SET peeks |
| `OP_CLOSE_UPVALUE slot` | closes every open upvalue whose stack slot is at/above `base + slot`, right before those locals are popped |
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
| `OP_TUPLE count` | pops `count` elems (left-to-right order), pushes a new tuple |
| `OP_STRUCT struct_slot` | pops `module->structs[slot]->field_count` elems (declaration order), pushes a struct instance |
| `OP_ENUM enum_slot tag` | pops that variant's `field_count` elems (declaration order), pushes an enum instance |
| `OP_FIELD_GET index` | pops a tuple/struct/enum instance, pushes its `index`-th field — no bounds check, since the index is always a compile-time-valid constant |
| `OP_TAG` | pops an enum instance, pushes its variant tag as `Int` |
| `OP_MATCH_FAIL` | runtime error "no match arm matched" — a backstop; the checker enforces exhaustiveness, but guards can still fail every arm |

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
| `OBJ_TUPLE` | `count`, `Value *items` — same shape as `OBJ_ARRAY`, kept as a distinct kind purely so printing/equality read as a tuple |
| `OBJ_STRUCT` | `StructDef *def`, `Value *fields` (`def->field_count` entries, declaration order — not necessarily the initializer's source order) |
| `OBJ_ENUM` | `VariantDef *variant`, `Value *fields` (`variant->field_count` entries, declaration order) |
| `OBJ_CLOSURE` | `FunDef *fun`, `ObjUpvalue **upvalues` (`upvalue_count` entries) — a capturing function value |
| `OBJ_UPVALUE` | `Value *location` (→ a live stack slot while open, → `closed` once closed), `closed`, `next` (open-list link) |

`StructDef`/`EnumDef`/`VariantDef` pointers are arena-owned (live for the
whole compilation), not GC objects, so marking a struct/enum instance walks
its `fields` array but never touches the def pointer itself.

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
`--gc-stress`. Roots: every compiled chunk's constant pool, scanned directly
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

Any allocating call (`heap_intern`, `heap_concat`, `heap_array`, `heap_tuple`,
`heap_struct`, `heap_enum`, `heap_closure`, `heap_upvalue`) may collect
*before* the new object exists, so
the discipline throughout codegen/VM is: keep every already-live operand
reachable from a root (still on the VM stack, not yet popped) until the new
object is built and pushed. See the `OP_ARRAY`/`OP_INTERP`/`OP_TUPLE`/
`OP_STRUCT`/`OP_ENUM` handlers in `src/vm.c` for the pattern (index math into
the stack in place, decrementing `sp` only after the result exists).

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
- Match (`compile_match`, see "Match compilation" below).
- Methods (`compile_method_call`) and associated functions/bare
  self-supplying calls (`Point::new(...)`, `Shape::area(s)`): see "Methods"
  below.
- `?` (`compile_propagate`): see "Propagate (`?`)" below.
- Closures (`compile_closure`): see "Closures & upvalues" below.
- Anything outside the subset (generic functions/methods, destructuring `var`
  bindings, and calls the checker resolved against a *trait* signature rather
  than an impl method — a call through a bound, or a default method the impl
  inherited, neither of which has a chunk) emits a source-anchored diagnostic
  `"... is not supported by the VM yet"` and fails codegen — it never
  crashes at runtime.

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

### Methods

Impl methods are ordinary `FunDef`s — `self` is just a regular `ParamDef`
with `is_self` set, at whatever position it was declared (checked
generically, not assumed to be first) — so they compile exactly like
top-level functions (`compile_fun_body`, shared by both). The only wrinkle
is *finding* the FunDef to compile: unlike a top-level `DeclFun`, an impl
item's `Decl` is never linked back to the `FunDef` `resolve_impl_decl`
builds into `ImplDef.methods[]` (`item->fun_decl->as.fun_decl.def` is never
set). `compile_impl` bridges the two by iterating `impl_decl->items` and
`impl_def->methods[]` in lockstep, exactly the order `resolve_impl_decl`
filled them in (method items only, skipping assoc-type items) — the same
implicit pairing the checker already relies on elsewhere.

Methods and top-level functions share one `OP_GET_GLOBAL` slot space
(`exe_link` numbers a module's methods right after its funs — see "Linking"),
so calling one needs no new opcode:

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

A generic impl's methods are rejected the same way a generic function is
(`impl_def->type_param_count > 0` — the impl's own type params can appear in
`Self`/param/return types, so this needs the same monomorphisation this VM
doesn't have yet); a generic method rejects itself via the same
`type_param_count` check `compile_fun_body` already runs for top-level
functions.

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

A closure expression (`|x| => body`) is compiled the crafting-interpreters
way: its body goes into its own `Chunk` via a *child* `Cg` whose `parent`
points at the enclosing function's `Cg`, and the enclosing chunk emits
`OP_CLOSURE` to build the runtime `ObjClosure` from the captured cells.

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
function, a struct whose constructor it calls. But every slot operand is a
single byte, so it cannot mean "index into my own module": `OP_GET_GLOBAL 3`
has to identify one function in the whole program.

`exe_link` (`src/codegen.c`) builds that flat namespace into an `Executable`
(`include/object.h`) before any code is generated:

| Table | Contents | Operand of |
|---|---|---|
| `globals[]` | per module: its top-level funs, then its impl methods | `OP_GET_GLOBAL` |
| `structs[]` | every module's structs | `OP_STRUCT` |
| `enums[]` | every module's enums (variant tags stay per enum) | `OP_ENUM` |
| `closures[]` | nested closure `FunDef`s, appended during codegen | nothing — GC roots only |

Each definition's assigned index is written back into its `slot`, so codegen
only ever emits `def->slot` and never asks which module a definition came
from. Modules are numbered in topological order; that is cosmetic (every slot
exists before the first chunk is compiled) but keeps a stack trace readable.
More than 256 functions, structs, or enums exceeds the operand width and is
reported as an error against the program rather than any one declaration.

Two things must happen in this order, and both are why linking is a separate
step rather than something `codegen_module` does on its way past:

- **All slots before any chunk.** Compiling module A can emit a slot for a
  definition in module B, so B's numbers must already be final — a
  compile-as-you-go scheme would need a patch-up pass over emitted bytecode.
- **The heap roots off the linked tables.** Codegen interns string literals,
  which can trigger a collection while most chunks are still empty. A `FunDef`
  in the tables with a NULL chunk is a root with nothing to mark; one missing
  from the tables is not a root at all, and its already-interned constants
  would be swept.

Codegen then runs per module (in topological order, purely so diagnostics
come out dependency-first), each reporting against its own source file. A
construct the VM doesn't support fails the whole program even in a module the
root never calls into — codegen compiles everything, and generic functions
still have no runtime representation.

Name resolution stays module-local: a bare `foo()` is compiled from
`ExprPath.resolved_fun`, the `FunDef` the *checker* picked, because the name
may be an alias (`use lib::helper as h;`) or belong to another module
entirely — a search over the enclosing module's own `funs[]` would find
neither.

## Future (design intent, not implemented)

- **Bytecode serialization** (the module system now exists): flat binary —
  magic/version, string table, recursive chunk records (name, arity, code,
  tagged constants, nested functions). Keep the constant pool free of raw
  pointers (string-table indices instead) so this stays a straight loop.
  Deliberately deferred: the chunk format is still changing.
- **REPL:** does not exist; would re-run the pipeline per line with a fresh
  arena, keeping the GC heap alive across lines.
