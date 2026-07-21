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
```

## Documentation

`references/` is the source of truth and is kept in sync with the code:
`overview.md` (layout, pipeline), `language.md` (syntax + semantics as
implemented, with the not-yet-implemented table), `architecture.md`
(scanner/parser/sema internals), `runtime.md` (codegen/VM), `roadmap.md`,
`grammar.ebnf`. **When you change syntax, pipeline behavior, or the runtime
subset, update the matching references/ file in the same change.** Old notes
in `.vscode/ref/` are historical — do not trust them.

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
