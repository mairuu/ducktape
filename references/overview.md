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
| `tests/warn/` | programs that must compile *and* warn (exit 0, stderr matching the first line's `#! expect: <substring>`) |
| `tests/fail/` | programs that must fail; first line `#! expect: <substring>` asserts on stderr |
| `tests/run/` | programs executed with `--run`; `#> line` comments assert on stdout |
| `tests/fail_run/` | like `tests/fail`, but invoked with `--run` — for programs that type-check yet the VM rejects |
| `std/` | the standard library, written in ducktape; embedded into the binary at build time. The bodies that cannot be (`print`, array/string/char primitives, hash mixing) are bodyless declarations bound to `src/native.c`. `std/lib.dt` is the library root and declares the tree — a file is a module only once a `mod` names it, and only public where that `mod` says `pub`. `--std-module std::cmp` compiles one *as* that module and warns; `make test` lints them all under the `tests/pass` rule |
| `scripts/run_tests.sh` | the test runner (invoked by `make test`). A `#! flags: …` line in a test file hands the compiler extra arguments — how the `-W` levels, which no source can set, are tested |
| `scripts/embed_std.sh` | mirrors `std/**/*.dt` into `build/std_data.h` for `src/std_src.c`, keyed by path relative to `std/`. A loader, not a resolver: what exists is what `std/lib.dt` declares |
| `editors/vscode/` | a VS Code extension: TextMate grammar + language config for `.dt` (highlighting only, no language server) |
| `references/` | these docs + `grammar.ebnf` |

A multi-file test is a *subdirectory* of any of the `tests/` categories, with
`main.dt` as the entry point and the modules it declares alongside. The flat
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
./build/ducktape -Werror file.dt            # every warning becomes an error
```

`main()` exits 0 only when every phase succeeded; diagnostics go to stderr.
`--run` tells an image from a source file by its magic bytes, not by the
extension.

`-W` sets a lint's level for the whole compile — `-Werror`, `-Werror=<lint>`,
`-Wno-<lint>`, `-W<lint>` — last one winning. It loses to an `@allow` on the
declaration and never escalates a warning std would have kept to itself; see
`language.md` "Warnings and `@allow`".

## Pipeline

Driver: `compiler_run` in `src/compiler.c`. Phases, in order:

1. **discover & parse** — build the two **module trees** and parse them. The
   program's is rooted at the file given on the command line, the library's at
   the embedded `std/lib.dt`; a module is in a tree because some module wrote
   `mod x;`, and where its source lives follows from its place there. Parsing
   (`src/scanner.c` + `src/parser.c` → AST, `include/ast.h`) is what reveals
   both a module's children and its dependencies, so the phase is a fixpoint:
   parse, let every `use` path register what it walks through, repeat until the
   registry stops growing, then resolve every `use` against the finished tree.
   Paths are absolute from their root, and nothing asks whether a file exists.
   The program's tree is its **build unit** — a declared module is compiled
   whether or not anything imports it — while a library module joins the build
   when a path reaches it. Each non-std module then gets the **prelude**
   (`mod_inject_prelude`): synthesised imports of the vocabulary/lang-item std
   modules (`Option`, `Result`, `Ord`, `Display`, and `std::string`), so those
   names and the lang items are in scope without an explicit `use`. They are
   ordinary import edges — the later phases don't know the prelude exists.
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
6. **unused items** — `tc_mark_live_items`: walk the item-use graph the two
   previous phases recorded, and report what it does not reach. Its own phase
   because an item's readers are anywhere in the tree, so this is the first
   moment the graph is complete

With `--run`, `compiler_execute` (`src/compiler.c`) additionally runs:

7. **tags** — `exe_assign_tags` (`src/codegen.c`): fix every enum's variant
   tags. All that is left of a linking step, since a tag is the one part of a
   program's layout that is not demand-driven
8. **codegen** — `src/codegen.c`: AST → bytecode `Chunk`, demand-driven from
   `main` outwards (`Mono`, drained until it reaches a fixpoint). A definition
   takes a slot and a chunk when something *reaches* it; one nothing reaches
   costs neither, generic or not. The program-wide slot tables (funs + methods,
   structs, enums, vtables) grow as that walk fills them
9. **vm** — `src/vm.c`: stack VM executes the root module's `main()`

`--emit-bc` stops after 8 and serializes the linked program instead
(`src/bytecode.c`); `--run` on an image skips 1–8 entirely, decoding straight
into the same `Executable` the VM would have been handed.

Diagnostics accumulate in a `DiagBag` (`src/diag.c`) shared by all phases; no
phase aborts mid-file. Error recovery uses a poison type/expr convention (see
`architecture.md`).

## Status snapshot

- **Type-checks:** the full grammar — generics, traits (with type parameters,
  associated types and supertraits) and impls, match with guards, closures, ranges, arrays, casts, interpolation,
  `?` propagation, and programs spanning several files via `use`. See
  `language.md` for the precise rules and the not-yet-implemented list.
- **Executes (`--run`):** nearly all of it — arithmetic, control flow,
  recursion, first-class function values and closures with upvalues, native
  functions (`std::io`, `std::array`, `std::string`, `std::char`),
  GC-backed strings (and the `StringBuf` that builds one), packed `Bytes` and
  the file read that fills one, growable arrays,
  structs/enums/tuples with full match compilation, methods and `?`,
  multi-module programs, and generic code via
  monomorphisation (including dispatch through a trait bound — by a receiver
  or by a path qualified with a type parameter — and inherited trait default
  bodies). What is left is listed in `language.md`'s
  not-yet-implemented table — nothing on it is a construct
  `tests/pass/in_fixed.dt` reaches. See `runtime.md`.
- **Showcase program:** `tests/pass/in_fixed.dt` exercises most of the checked
  language in one file.
- **Roadmap:** `roadmap.md`.
