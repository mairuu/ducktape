# ducktape — project overview

ducktape is a toy statically-typed, Rust-inspired language (`.dt` files). The
compiler and VM are written in C23 and built with GNU Make. The frontend
(scanner → parser → type checker) covers essentially the whole language; the
backend (bytecode + stack VM) executes the value-type subset plus GC-backed
strings and arrays, with structs/enums/closures still ahead (`roadmap.md`).

Everything in `references/` is written against the actual code — when a claim
names a file, that file is the source of truth. Historical design notes live in
`.vscode/ref/` (gitignored) and are superseded by these documents.

## Repo layout

| Path | Contents |
|---|---|
| `src/`, `include/` | the compiler + VM, one `.c`/`.h` pair per component (incl. `object.{c,h}`: the GC heap, strings, arrays) |
| `tests/pass/` | programs that must compile cleanly (exit 0, empty stderr) |
| `tests/fail/` | programs that must fail; first line `#! expect: <substring>` asserts on stderr |
| `tests/run/` | programs executed with `--run`; `#> line` comments assert on stdout |
| `scripts/run_tests.sh` | the test runner (invoked by `make test`) |
| `references/` | these docs + `grammar.ebnf` |

## Build, test, run

```sh
make               # build build/ducktape (BUILD=release for -O2)
make test          # build + run the whole test suite
./build/ducktape file.dt                    # compile (type-check) only
./build/ducktape --run file.dt              # compile and execute main()
./build/ducktape --run --gc-stress file.dt  # + collect on every allocation
```

`main()` exits 0 only when every phase succeeded; diagnostics go to stderr.

## Pipeline

Driver: `compiler_run` in `src/compiler.c`. Phases, in order:

1. **discover** — collect source files (currently: exactly the one file given)
2. **parse** — `src/scanner.c` + `src/parser.c` → AST (`include/ast.h`)
3. **dep graph** — placeholder topological order (single module)
4. **register** — `tc_register_module` (`src/sema.c`): create def stubs for
   top-level declarations, register builtins
5. **resolve** — `tc_resolve_module`: resolve signatures, types, impls
6. **check** — `tc_check_module`: type-check function bodies with inference

With `--run`, `compiler_execute` (`src/compiler.c`) additionally runs:

7. **codegen** — `src/codegen.c`: AST → bytecode `Chunk` per function
8. **vm** — `src/vm.c`: stack VM executes `main()`

Diagnostics accumulate in a `DiagBag` (`src/diag.c`) shared by all phases; no
phase aborts mid-file. Error recovery uses a poison type/expr convention (see
`architecture.md`).

## Status snapshot

- **Type-checks:** the full grammar — generics, traits/impls with associated
  types, match with guards, closures, ranges, arrays, casts, interpolation,
  `?` propagation. See `language.md` for the precise rules and the
  not-yet-implemented list.
- **Executes (`--run`):** Int/Float/Bool/unit/ranges/functions — arithmetic,
  control flow, recursion, first-class function values, `print` — plus
  GC-backed strings (interning, `+` concat, interpolation) and arrays
  (literals, indexing, index assignment, `for x in arr`). Structs, enums,
  match, and closures compile but are rejected by codegen with "not
  supported by the VM yet" until the next runtime milestone. See
  `runtime.md`.
- **Showcase program:** `tests/pass/in_fixed.dt` exercises most of the checked
  language in one file.
- **Roadmap:** `roadmap.md`.
