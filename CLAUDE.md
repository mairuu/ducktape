# ducktape

Toy statically-typed, Rust-inspired language; compiler + bytecode VM in C23.

## Commands

```sh
make               # build build/ducktape
make test          # build + full test suite (must stay green)
./build/ducktape file.dt         # compile / type-check only
./build/ducktape --run file.dt   # compile and execute main()
./build/ducktape --emit-bc out.dtbc file.dt   # compile to a bytecode image
./build/ducktape --run out.dtbc               # execute an image
./build/ducktape -Werror file.dt              # lint levels: -Werror[=<lint>],
                                              # -Wno-<lint>, -W<lint>
make format        # clang-format over src/ and include/
make sanitize      # rebuild under ASan+UBSan and run the suite
```

A sanitizer finding is not a test failure — `make test` stays green while the
binary does something undefined — so run `make sanitize` after touching memory
management (the arena, the heap, any raw buffer) and before landing it.

## Documentation

`references/` is the source of truth and is kept in sync with the code:
`overview.md` (layout, pipeline), `language.md` (syntax + semantics as
implemented, with the not-yet-implemented table), `architecture.md`
(scanner/parser/sema internals), `runtime.md` (codegen/VM), `roadmap.md`,
`grammar.ebnf`. **When you change syntax, pipeline behavior, or the runtime
subset, update the matching references/ file in the same change.** Old notes
in `.vscode/ref/` are historical — do not trust them.
Completed milestones through 75 are archived in `references/history/` (two
files, split at 54); the roadmap keeps 76 on. Archive again when it passes
~150KB.

### One narrative, one home

A milestone's full write-up goes in **the commit body**, once. Everything else
points at it:

- `roadmap.md` Done entry: ~8 lines — what shipped, the SHA, which
  `references/` sections cover it, the one finding, what's left over.
- A wart the milestone closes is **deleted** from "Known warts", not struck
  through with the story of closing it. If it left a remainder, that remainder
  becomes its own open entry, written as the gap it now is.
- `references/` prose: what the language/compiler *does now*, not the story of
  arriving at it. No milestone narration.
- A test file: one or two lines on what it pins. Rationale lives in
  `references/`.
- A new code comment: state the non-obvious invariant and stop. No 8-line
  block over a 10-line function; existing comments stay as they are.

Prose is output tokens, which cost several times what reading does, so the same
paragraph written three times is the most expensive habit available. Prefer
short. Don't re-explain what the diff already shows.

## Standard library

`std/*.dt` is the standard library, **written in ducktape**. Each file is an
ordinary module that `scripts/embed_std.sh` mirrors into `build/std_data.h` at
build time, so `use std::cmp;` needs no install path and the test suite stays
hermetic. Edit the `.dt`; never edit the generated header. `std/lib.dt` is the
library root and declares the tree: a new file needs a `pub mod` there (or in
its group's file) before `use std::<name>;` resolves. A plain `mod` puts the
file in the library without exporting it — `std::collections`'s children are
declared that way, reachable only from inside the group.

**`./build/ducktape --std-module std::cmp` lints one std module**: it compiles
as the library module its *path* names, reading the source from disk (so you
see the edit, not the last `make`), and a root always has a warning audience —
where a program that merely uses std never sees its warnings. `make test` lints
every module under the `tests/pass` rule, so a warning in std fails the suite.
A file no `mod` declares is not a module at all, and now warns
(`orphan_module`) instead of going quiet.

## Tests

- `tests/pass/*.dt` — must compile: exit 0, empty stderr.
- `tests/fail/*.dt` — must fail; first line `#! expect: <substring>` is
  matched against stderr.
- `tests/run/*.dt` — executed with `--run`; `#> line` comments are the
  expected stdout, in order.
- `tests/fail_run/*.dt` — like `tests/fail`, but invoked with `--run`; for
  programs that type-check yet the VM rejects.
- `tests/warn/*.dt` — must compile (exit 0) with *non-empty* stderr containing
  the `#! expect:` substring; the one bucket a warning fits.
- A `#! flags: <args>` line anywhere in a pass/warn/fail file hands the
  compiler extra arguments — how a `-W` lint level, which no source can set,
  gets tested.
- A multi-file test is a *subdirectory* of any of those, entry point
  `main.dt`, imported modules alongside it. The flat globs are non-recursive,
  so the siblings are never collected as tests themselves.
- Add a test with every feature or fix. `tests/pass/in_fixed.dt` is the
  full-language showcase.
- Every `tests/run` program is also emitted as a bytecode image and re-run
  from it, so anything the image format forgets fails as an output diff.

## Conventions

- All allocation goes through the `Allocator` vtable; the compiler owns one
  arena freed at destroy — no bare malloc/free in compiler code.
- Checker errors: emit one `diag_error(diags, span, ...)` (types printed via
  `type_sprintf` into a `char buf[64]`) and return `t_poison`; poison
  propagates silently so one mistake yields one diagnostic.
- Type identity is pointer equality (`types_equal`); structural types are
  interned — always build them via the `ty_*` constructors.
- Unification (`infer_unify`) emits its own mismatch diagnostic; the
  "expected" type goes first.
- New `resolve_expr` cases go before the `default:` assert; large ones become
  `static Type *resolve_<kind>_expr(CheckCtx *, Expr *, Type *hint)` helpers.
- Codegen must never crash on unhandled constructs — emit the
  "... is not supported by the VM yet" diagnostic and fail compilation.
- Build must stay warning-free (`-Wall -Wextra -Wpedantic`); adding a
  `TypeKind`/`ExprKind` means updating every switch the compiler flags.
