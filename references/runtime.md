# ducktape — runtime (codegen + VM)

`./build/ducktape --run file.dt` compiles the checked AST to bytecode and
executes `main()`. Entry: `compiler_execute` (`src/compiler.c`) →
`codegen_module` (`src/codegen.c`) → `vm_run` (`src/vm.c`).

## Values (`include/value.h`)

Tagged union — value types only until the GC exists:

| Kind | Payload |
|---|---|
| `VAL_INT` | `int64_t` |
| `VAL_FLOAT` | `double` |
| `VAL_BOOL` | `bool` |
| `VAL_UNIT` | — |
| `VAL_RANGE` | start, end (`int64_t`), inclusive flag |
| `VAL_FUN` | `FunDef *` (top-level function, no captures) |

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

## Codegen shapes (`src/codegen.c`)

- Locals are compile-time tracked (`Cg.locals`, name → frame slot); the
  initializer's stack slot *is* the variable. Params occupy slots 0..n-1.
- Blocks: compile stmts, compile tail (or `OP_UNIT`), then `OP_SLIDE n`.
- `and`/`or` compile to jump-based short-circuit; `if` always produces a
  value (`OP_UNIT` for a missing else).
- Loops evaluate to unit. `for x in <range>` materializes two locals —
  hidden `range` and the loop variable — then tests with `OP_RANGE_TEST` and
  increments by constant 1. `break`/`continue` emit `OP_POPN` down to the
  loop's recorded local base (continue keeps the hidden iter locals, break
  does not) before jumping; forward continue targets the increment.
- Calls: push callee (`OP_GET_GLOBAL` or a local), args left-to-right,
  `OP_CALL`. A call to the unshadowed name `print` lowers to `OP_PRINT`.
- Anything outside the subset (strings, interpolation, arrays, indexing,
  structs, enums, match, closures, methods, `?`, tuples, generic functions,
  destructuring) emits a source-anchored diagnostic
  `"... is not supported by the VM yet"` and fails codegen — it never
  crashes at runtime.

## VM (`src/vm.c`)

Fixed arrays: 4096-value stack, 128 call frames. A frame is
`{FunDef*, ip, base}` with `base` pointing at arg slot 0; the callee value
lives at `base - 1`. Runtime errors (division by zero, stack overflow,
calling a non-function) print `runtime error: ...` plus a call trace of
function names and make the process exit 1. The checker guarantees operand
kinds, so opcode handlers don't re-validate types.

## Future (design intent, not implemented)

- **GC milestone (5b):** mark-sweep heap for `ObjString` (interned; enables
  `String` runtime, `+` concat, interpolation) and arrays; then structs,
  enums, match dispatch, and closures-with-upvalues (5c) — `VarEntry
  .is_captured` is already maintained by the checker for this.
- **Bytecode serialization** (after a module system exists): flat binary —
  magic/version, string table, recursive chunk records (name, arity, code,
  tagged constants, nested functions). Keep the constant pool free of raw
  pointers (string-table indices instead) so this stays a straight loop.
  Deliberately deferred: the chunk format is still changing.
- **REPL:** does not exist; would re-run the pipeline per line with a fresh
  arena, keeping the GC heap alive across lines.
