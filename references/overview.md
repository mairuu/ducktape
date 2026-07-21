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
| `tests/fail_run/` | like `tests/fail`, but invoked with `--run` — for programs that type-check yet the VM rejects |
| `scripts/run_tests.sh` | the test runner (invoked by `make test`) |
| `references/` | these docs + `grammar.ebnf` |

A multi-file test is a *subdirectory* of any of the `tests/` categories, with
`main.dt` as the entry point and its imported modules alongside. The flat
`*.dt` globs are non-recursive, so those siblings are never collected as tests
in their own right.

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

1. **discover & parse** — a worklist rooted at the file given on the command
   line: parse a module (`src/scanner.c` + `src/parser.c` → AST,
   `include/ast.h`), resolve its `use` declarations to files, and continue into
   any module that just appeared. Discovery can't precede parsing, because a
   module's dependencies *are* its `use` declarations. Paths resolve against
   the root file's directory, which is the one module search root.
2. **dep graph** — tri-colour DFS over the import edges, post-order, so
   dependencies precede their dependents; a back edge is a cycle diagnostic
3. **register** — `tc_register_module` (`src/sema.c`): create def stubs for
   top-level declarations, register builtins
4. **link imports + resolve** — per module in topological order:
   `tc_link_imports` copies each `use`d item into the module's scopes, then
   `tc_resolve_module` resolves signatures, types, impls
5. **check** — `tc_check_module`: type-check function bodies with inference

With `--run`, `compiler_execute` (`src/compiler.c`) additionally runs:

6. **link** — `exe_link` (`src/codegen.c`): flatten every module's funs,
   methods, structs and enums into one program-wide slot space
7. **codegen** — `src/codegen.c`: AST → bytecode `Chunk` per function, per
   module
8. **vm** — `src/vm.c`: stack VM executes the root module's `main()`

Diagnostics accumulate in a `DiagBag` (`src/diag.c`) shared by all phases; no
phase aborts mid-file. Error recovery uses a poison type/expr convention (see
`architecture.md`).

## Status snapshot

- **Type-checks:** the full grammar — generics, traits/impls with associated
  types, match with guards, closures, ranges, arrays, casts, interpolation,
  `?` propagation, and programs spanning several files via `use`. See
  `language.md` for the precise rules and the not-yet-implemented list.
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
