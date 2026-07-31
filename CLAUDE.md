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
Completed milestones through 54 are archived in `references/history/`; the
roadmap keeps 55 on. Archive again when it passes ~150KB.

### One narrative, one home

A milestone's full write-up goes in **the commit body**, once. Everything else
points at it:

- `roadmap.md` Done entry: ~8 lines — what shipped, the SHA, which
  `references/` sections cover it, the one finding, what's left over.
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
ordinary module (directly runnable: `./build/ducktape std/cmp.dt`) that
`scripts/embed_std.sh` mirrors into `build/std_data.h` at build time, so
`use std::cmp;` needs no install path and the test suite stays hermetic. Edit
the `.dt`; never edit the generated header. Adding a file to `std/` is all it
takes to make `use std::<name>;` resolve.

## Tests

- `tests/pass/*.dt` — must compile: exit 0, empty stderr.
- `tests/fail/*.dt` — must fail; first line `#! expect: <substring>` is
  matched against stderr.
- `tests/run/*.dt` — executed with `--run`; `#> line` comments are the
  expected stdout, in order.
- `tests/fail_run/*.dt` — like `tests/fail`, but invoked with `--run`; for
  programs that type-check yet the VM rejects.
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
