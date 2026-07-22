# ducktape — project overview

ducktape is a toy statically-typed, Rust-inspired language (`.dt` files). The
compiler and VM are written in C23 and built with GNU Make. The frontend
(scanner → parser → type checker) covers essentially the whole language, and
the backend (bytecode + stack VM) now runs all of it: GC-backed strings,
arrays, structs, enums, closures, generics (monomorphised), trait objects and
multi-module programs. A small standard library is written in ducktape itself
and embedded in the binary (`std/`). See `roadmap.md` for what is left.

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
| `std/` | the standard library, written in ducktape; embedded into the binary at build time. The bodies that cannot be (`print`, array/string/char primitives) are bodyless declarations bound to `src/native.c` |
| `scripts/run_tests.sh` | the test runner (invoked by `make test`) |
| `scripts/embed_std.sh` | mirrors `std/*.dt` into `build/std_data.h` for `src/std_src.c` |
| `editors/vscode/` | a VS Code extension: TextMate grammar + language config for `.dt` (highlighting only, no language server) |
| `references/` | these docs + `grammar.ebnf` |

A multi-file test is a *subdirectory* of any of the `tests/` categories, with
`main.dt` as the entry point and its imported modules alongside. The flat
`*.dt` globs are non-recursive, so those siblings are never collected as tests
in their own right.

`editors/vscode/syntaxes/ducktape.tmLanguage.json` duplicates the keyword
tables of `src/scanner.c` — a new keyword has to be added in both places, or it
simply renders as an identifier.

## Build, test, run

```sh
make               # build build/ducktape (BUILD=release for -O2)
make test          # build + run the whole test suite
./build/ducktape file.dt                    # compile (type-check) only
./build/ducktape --run file.dt              # compile and execute main()
./build/ducktape --run --gc-stress file.dt  # + collect on every allocation
./build/ducktape --emit-bc prog.dtbc file.dt # compile to a bytecode image
./build/ducktape --run prog.dtbc            # run an image (no source needed)
```

`main()` exits 0 only when every phase succeeded; diagnostics go to stderr.
`--run` tells an image from a source file by its magic bytes, not by the
extension.

## Pipeline

Driver: `compiler_run` in `src/compiler.c`. Phases, in order:

1. **discover & parse** — a worklist rooted at the file given on the command
   line: parse a module (`src/scanner.c` + `src/parser.c` → AST,
   `include/ast.h`), resolve its `use` declarations to files, and continue into
   any module that just appeared. Discovery can't precede parsing, because a
   module's dependencies *are* its `use` declarations. Paths resolve against
   the root file's directory, which is the one module search root — except
   `use std::…`, which names a module embedded in the binary and takes its
   source from there instead of the filesystem. It is an ordinary module in
   every other respect.
2. **dep graph** — tri-colour DFS over the import edges, post-order, so
   dependencies precede their dependents; a back edge is a cycle diagnostic
3. **register** — `tc_register_module` (`src/sema.c`): create def stubs for
   top-level declarations, and bind each `@native`/`@intrinsic` declaration to
   C's registry (`src/native.c`)
4. **link imports + resolve** — per module in topological order:
   `tc_link_imports` copies each `use`d item into the module's scopes,
   `tc_import_impls` unions each dependency's visible impls into it, then
   `tc_resolve_module` resolves signatures, types, impls
5. **check** — `tc_check_module`: type-check function bodies with inference

With `--run`, `compiler_execute` (`src/compiler.c`) additionally runs:

6. **tags** — `exe_assign_tags` (`src/codegen.c`): fix every enum's variant
   tags. All that is left of a linking step, since a tag is the one part of a
   program's layout that is not demand-driven
7. **codegen** — `src/codegen.c`: AST → bytecode `Chunk`, demand-driven from
   `main` outwards (`Mono`, drained until it reaches a fixpoint). A definition
   takes a slot and a chunk when something *reaches* it; one nothing reaches
   costs neither, generic or not. The program-wide slot tables (funs + methods,
   structs, enums, vtables) grow as that walk fills them
8. **vm** — `src/vm.c`: stack VM executes the root module's `main()`

`--emit-bc` stops after 7 and serializes the linked program instead
(`src/bytecode.c`); `--run` on an image skips 1–7 entirely, decoding straight
into the same `Executable` the VM would have been handed.

Diagnostics accumulate in a `DiagBag` (`src/diag.c`) shared by all phases; no
phase aborts mid-file. Error recovery uses a poison type/expr convention (see
`architecture.md`).

## Status snapshot

- **Type-checks:** the full grammar — generics, traits/impls with associated
  types, match with guards, closures, ranges, arrays, casts, interpolation,
  `?` propagation, and programs spanning several files via `use`. See
  `language.md` for the precise rules and the not-yet-implemented list.
- **Executes (`--run`):** nearly all of it — arithmetic, control flow,
  recursion, first-class function values and closures with upvalues, native
  functions (`std::io::print`, `std::array`, `std::string`, `std::char`),
  GC-backed strings (and the `StringBuf` that builds one) and growable arrays,
  structs/enums/tuples with full match compilation, methods and `?`,
  multi-module programs, and generic code via
  monomorphisation (including dispatch through a trait bound and inherited
  trait default bodies). What is left is listed in `language.md`'s
  not-yet-implemented table — nothing on it is a construct
  `tests/pass/in_fixed.dt` reaches. See `runtime.md`.
- **Showcase program:** `tests/pass/in_fixed.dt` exercises most of the checked
  language in one file.
- **Roadmap:** `roadmap.md`.
