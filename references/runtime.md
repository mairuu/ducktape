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
| `OP_MAKE_DYN vtable_slot` | pops a value, pushes it wrapped as a trait object carrying `exe->vtables[slot]` |
| `OP_DYN_METHOD index` | pops a trait object, pushes its `index`-th method *then* the unwrapped receiver — so the `OP_CALL` that follows sees an ordinary callee-beneath-args stack |
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
| `OBJ_DYN` | `Value inner` (the coerced value, stored as-is), `VTable *vtable` — a trait object |

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
- Generic functions, methods and impls (`compile_fun_body` under a `Subst`):
  see "Monomorphisation" below.
- Destructuring `var` bindings (`compile_destructure`): see "Match
  compilation" below.
- Trait default bodies the impl inherited (`compile_method_call` again, via
  `TraitMethodDef.default_impl`): see "Monomorphisation" below.
- Anything outside the subset (`print` named as a value rather than called,
  say) emits a source-anchored diagnostic
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

### Methods

Impl methods are ordinary `FunDef`s — `self` is just a regular `ParamDef`
with `is_self` set, at whatever position it was declared (checked
generically, not assumed to be first) — so they compile exactly like
top-level functions (`compile_fun_body`, shared by both).

`codegen_module` walks the module's *definition tables* (`Module.funs[]`,
then each `ImplDef.methods[]`), not its AST. A `FunDef` carries its own body
and span, which is what monomorphisation needs — a call site in another
module reaches a generic definition through the `FunDef` alone and has no
`Decl` to consult. It also removes the old lockstep pairing between
`impl_decl->items` and `impl_def->methods[]`, which was needed only because
an impl item's `Decl` is never linked back to its `FunDef`
(`item->fun_decl->as.fun_decl.def` is never set).

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

- `exe_link` gives a generic definition **no slot** — there is no single body
  to address — and `codegen_module` skips it. An uncalled generic therefore
  costs nothing and is not an error.
- A call site whose target is generic asks `mono_request` for the copy keyed
  by its type arguments. First request allocates a `FunDef` clone (same body,
  params and name; its own slot appended to `exe->globals`, its own chunk)
  and queues it; later requests return the same one, which is what makes a
  recursive generic terminate.
- The queue is drained after every module has been walked. Draining can
  enqueue more, so the driver loops on `mono_pending_module` until empty, and
  reports each instance against the module its *body* was written in, not the
  one that instantiated it.

The type arguments come from the checker: `resolve_callee`,
`resolve_method_call_expr` and `check_trait_method_call` each stash the
`Subst` they solved onto the call node (`ExprPath.inst`,
`ExprMethodCall.inst`), rewritten to concrete types by `cctx_solve_insts`
once `infer_finalize` has run. Those arguments are in the *enclosing*
definition's terms, so a call inside a generic body is instantiated by
pushing the caller's own bindings (`Cg.subst`) through them — the step that
lets `describe_twice<T>` reach `describe<Int>` reach `Int::show` rather than
stopping one level down.

Two call shapes need more than the recorded arguments:

- **A method of a generic impl.** Its body mentions the impl's type params as
  well as its own, so `cg_inst_key` binds both, impl's first, dropping any
  the method's own shadow by name — the same rule `subst_exclude_shadowed`
  applies in the checker, and for the same reason (`subst_apply` matches by
  name and takes the first hit).
- **A call through a trait bound.** The checker resolved `v.show()` against
  the trait *signature*, so there is no `MethodDef` on the node — only
  `bound_trait` and `bound_self`. Codegen substitutes `bound_self` into a
  concrete type and re-runs `impl_index_method` to find the body, falling back
  to `impl_index_default_method` when the impl inherited it. This is the one
  place codegen consults the impl index, and it is why `Mono` holds one.

**Inherited default bodies** ride the same machinery. A trait method's default
body gets a `FunDef` of its own at resolve time
(`resolve_trait_default_impl` → `TraitMethodDef.default_impl`) whose *first
type parameter is `Self`*, bounded by the trait:

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
alongside the method's own type arguments, which is the whole key
`cg_inst_key` needs. An impl method has no parameter of that name, so on the
calls that land on one the extra binding is simply unused.

`MONO_MAX_DEPTH` (32) bounds the chain. `fun grow<T>(v: T) { grow([v]) }`
type-checks but names a *different* instantiation at every level, each keyed
one array deeper than the last, so nothing converges. The one-byte slot space
would stop it eventually, but only after interning a few hundred new types
into a fixed-size intern table — so the limit lives where the divergence is.

### Trait objects

Monomorphisation rests on the observation that *the runtime is uniform in
type arguments*, so a type argument changes exactly one thing: which function
a call resolves to. A trait object is the case where that choice cannot be
made at compile time — so it is **carried by the value**. A `dyn Shape` is
the pair `(value, the table of slots monomorphisation would have picked)`.
That is the whole feature; nothing else about the representation changes.

A `VTable` (`include/object.h`) is one `FunDef *` per method the trait
declares, in declaration order, already resolved and monomorphised for one
concrete self type. Filling a slot is the same two-way choice
`compile_method_call` makes for a bound receiver — the impl's own method, or
the trait's default body instantiated at `Self` = that type
(`cg_dyn_slot_target`) — just made ahead of time for every method at once.

Vtables live in `exe->vtables`, appended during codegen exactly as
monomorphised globals are, and for the same reason: which `(trait, type)`
pairs a program needs is a property of its coercion sites. `Mono.vtables`
memoises the compile-time key so two coercions of one type to one trait share
a table; the slot is reserved *before* the table is filled, since compiling a
method body can reach the same pair again (`mono_request` does the same).
Note what the runtime `VTable` does **not** hold: the trait and the self type.
Those are compiler bookkeeping the VM never reads — the serialization rule,
one level over.

Two opcodes, and deliberately no third:

- `OP_MAKE_DYN <vtable>` pops a value and pushes it wrapped as an `ObjDyn`.
  Codegen emits it from `compile_expr`, which wraps every expression so a
  coercion the checker recorded on a node cannot be missed by whichever of
  the ~30 expression cases produced the value.
- `OP_DYN_METHOD <index>` pops the trait object and pushes its method's
  function *then* the unwrapped receiver. That leaves the stack in the shape
  `OP_CALL` already understands — callee beneath its arguments, concrete
  receiver as argument zero — so dynamic dispatch needs no call machinery of
  its own, the same way methods reuse `OP_GET_GLOBAL`.

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
function, a struct whose constructor it calls. But every slot operand is a
single byte, so it cannot mean "index into my own module": `OP_GET_GLOBAL 3`
has to identify one function in the whole program.

`exe_link` (`src/codegen.c`) builds that flat namespace into an `Executable`
(`include/object.h`) before any code is generated:

| Table | Contents | Operand of |
|---|---|---|
| `globals[]` | per module: its top-level funs, then its impl methods; then monomorphised instances | `OP_GET_GLOBAL` |
| `structs[]` | every module's structs | `OP_STRUCT` |
| `enums[]` | every module's enums (variant tags stay per enum) | `OP_ENUM` |
| `closures[]` | nested closure `FunDef`s, appended during codegen | nothing — GC roots only |

Each definition's assigned index is written back into its `slot`, so codegen
only ever emits `def->slot` and never asks which module a definition came
from. Modules are numbered in topological order; that is cosmetic (every slot
exists before the first chunk is compiled) but keeps a stack trace readable.
More than 256 functions, structs, or enums exceeds the operand width and is
reported as an error against the program rather than any one declaration.

`globals[]` is the one table sized to the whole operand space rather than to
its contents: generic definitions get `FUN_SLOT_NONE` here and their
instances are appended during codegen (see "Monomorphisation"), so the count
is not known until compilation is over.

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
come out dependency-first), each reporting against its own source file, and
the monomorphisation queue is drained afterwards. A construct the VM doesn't
support fails the whole program even in a module the root never calls into —
codegen compiles every non-generic definition whether or not it is reachable.
A *generic* definition is the exception: it is only compiled where it is
instantiated, so one nobody calls is never looked at.

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
header   "DTBC", u16 version, u16 entry (a globals index)
counts   u32 strings, globals, structs, enums, closures, vtables
strings  [u32 len, len bytes]*        — every name and string constant
structs  [u32 name, u8 is_tuple, u16 field_count, u32 field_name*]
enums    [u32 name, u16 variant_count,
            [u32 name, u8 is_tuple, u16 field_count, u32 field_name*]*]
vtables  [u16 method_count, u16 fun_index*]*
funs     [u32 name, u16 param_count, u8 has_chunk,
            u32 code_len, code_len bytes,
            u16 const_count, const*]*  — globals first, then closures
```

Every multi-byte field is little-endian and written byte by byte, so an image
depends on neither the host's byte order nor any struct layout. Floats go out
as their IEEE-754 bit pattern.

**An image is the runtime projection of the program.** Only what `vm.c` and
`value_print` actually read off a `FunDef`/`StructDef`/`EnumDef` is written —
names, arities, field shapes, chunks. Types, spans and the owning `Module` are
compile-time concerns, so a loaded def has them NULL: an image has no AST
behind it, and nothing in the VM asks for one.

**The pool holds no raw pointers.** A string constant becomes a string-table
index, a `VAL_FUN` constant an index into the funs section (globals, then
closures — a nested closure function is reachable only through a constant, so
it has no slot of its own). That is what keeps both directions a straight
loop. Struct and enum references need no encoding at all: `OP_STRUCT`/`OP_ENUM`
already carry a slot the VM resolves through `exe`, and the tables are written
in slot order. Variant tags are positional, exactly as `exe_link` assigns them.
A vtable is likewise written as bare function indices: which trait and which
self type it was built for is compile-time bookkeeping, so it is not in the
image at all. Vtables sit between the data definitions and the code — they
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
sanity-checked against the one-byte operand ceiling, so a truncated or corrupt
image is rejected rather than executed. `tests/run` doubles as the round-trip
suite: `scripts/run_tests.sh` emits an image for every runnable program and
re-runs it, so a construct the format forgets shows up as an output diff.

## Future (design intent, not implemented)

- **REPL:** does not exist; would re-run the pipeline per line with a fresh
  arena, keeping the GC heap alive across lines.

### Native functions

`print` is currently special in three unrelated ways: `tc_register_builtins`
hand-builds its `FunDef` in C, codegen pattern-matches the call into a
dedicated `OP_PRINT`, and `cg_names_builtin_print` exists only to chase
`use std::io::print as p;` aliases. None of that scales — an opcode per native
spends a one-byte opcode space, and `print` still cannot be used as a value
(`cg_call_target` refuses it: a builtin has no body to point a slot at).

The intended replacement is one field and one branch.

**Mechanism.** `FunDef` gains a `NativeFn native`, with exactly one of
`chunk`/`native` non-NULL. `OP_CALL` gets a branch where it currently rejects
a NULL chunk, calling the C function and pushing its result without opening a
frame. Natives take **ordinary global slots**, so `OP_GET_GLOBAL` works on them
and they are first-class for free: passable as values, storable in arrays,
capturable by closures, and usable as vtable entries — `OP_DYN_METHOD` pushes
a `FunDef *` and `OP_CALL` does not care what is behind it.

**Surface.** A *bodyless* declaration in an ordinary std module, so the
signature is written in ducktape and the checker needs no special path at all:

```
@native("dt_sqrt")   pub fun sqrt(x: Float) -> Float;
@intrinsic("len")    pub fun len<T>(xs: [T]) -> Int;
```

C supplies only a name → function-pointer registry, resolved at link time; an
unknown name is a compile error with a real span, which a hand-built builtin
can never have. A body next to a native binding would read as "which one
wins?", so there is none — the attribute *is* the body.

Three tiers, one declaration surface: **`@intrinsic`** lowers inline to an
opcode (this is what would finally expose `OP_LEN`, today reachable only from
the `for` desugaring), **`@native`** becomes a call, and everything expressible
stays plain ducktape, where `std::cmp` already sits.

**A generic native needs no monomorphisation.** The runtime is uniform in type
arguments, so `print<T>` has one body for every `T`; `cg_call_target` returns it
directly rather than keying a copy. This is the monomorphisation observation
paying out one more time.

Two parts need designing rather than assuming:

- **GC calling convention.** A native that allocates can trigger a collection,
  and the VM stack is the root set — so arguments must stay on the stack across
  the call, with the native receiving a `Value *` into it and the result pushed
  only afterwards. `OP_MAKE_DYN` already follows exactly this discipline for
  `heap_dyn`; copy it rather than reinvent it. Getting it wrong reproduces the
  5c-ii class of bug: something reachable-looking the collector cannot see.
- **Serialization.** A C function pointer cannot go in an image, and an image
  is the runtime projection of the program with no compiler behind it. So a
  native is written **by name** and `bc_load` re-binds it against the running
  binary's registry. That couples an image to a native ABI rather than to
  `BC_VERSION` alone — an unknown name at load is a clean error, but the header
  probably wants a registry hash so it fails at load instead of at first call.

The milestone should be **net-negative lines in the compiler**. Porting `print`
to it deletes `OP_PRINT`, `cg_names_builtin_print`, the hand-built `FunDef` in
`tc_register_builtins`, the "using a builtin as a value" diagnostic, and the
`std::io` no-op special case in `mod_collect_imports`/`tc_link_imports`. If it
is not net-negative, the design is wrong.

Rejected, with reasons: **inline C in `.dt` files** (needs a compiler at runtime
or a per-program build step, which destroys the hermetic embedded std);
**a separate `OP_NATIVE` index space** (dodges slot pressure but costs
first-class-ness); **variadics** (the VM asserts `param_count == argc`, and
relaxing it touches frame setup for a feature only `print`-alikes want).
