# ducktape — roadmap

## Done

- **Frontend core** — scanner, parser, AST for the full grammar; three-pass
  semantic analysis with union-find inference, generics, impls, pattern
  matching.
- **Checker completion** — every parsed expression kind checks: unary,
  ranges (`TY_RANGE`), casts, interpolation, arrays (`[T]` types + literals +
  indexing), pipe closures with hint-driven inference, structural `?`,
  associated types in impls, `return` + `TY_NEVER`; trait/`use` declarations
  tolerated (registration-only). Test harness (`make test`) with pass/fail
  suites.
- **Generic inherent-impl inference** — bare `Point::new(..)` selects the
  impl by method name and infers type args at the call site.
- **VM core (milestone 5a)** — `Value`, `Chunk`, codegen for the value-type
  subset, stack VM with call frames, `print` builtin, `--run` flag,
  `tests/run` output-checked suite.
- **GC + heap objects (milestone 5b)** — mark-sweep collector rooted off the
  VM stack + chunk constants; interned `ObjString` → `String` runtime, `+`
  concat, interpolation (`OP_INTERP`); heap arrays (`ObjArray`) → literals,
  indexing, index assignment, `for x in arr`; `--gc-stress` flag. Design:
  `runtime.md` "Heap & GC". Also fixed two pre-existing bugs surfaced by
  exercising the feature: `String + String` was dead code in the checker
  (arithmetic's numeric check ran first), and the parser dropped text
  segments between two interpolation groups (`"{a} mid {b}"`).

## Next (in recommended order)

Estimates are relative to one focused session ≈ the checker-completion
milestone (~900 lines).

1. **5c — aggregate runtime** (~1–1.5×): structs/enums as heap objects, field
   access, methods (`self` dispatch), match compilation (tag dispatch +
   destructuring), closures with upvalues (`is_captured` groundwork exists),
   `?` lowering, tuples.
2. **Trait completion** (~0.75×): resolve trait-item signatures, impl
   conformance checking, default methods, calls through bounds
   (`T: Drawable` → `t.draw()`); then inline bounds `<T: Display>` and
   `where` enforcement (~0.5×). Unlocks `Try`-trait `?` and `as` coercions.
3. **Module system** (~0.5×): real file discovery, `mod_link_imports`
   (stubbed at `src/compiler.c` phase_register), cross-module visibility,
   cycle detection.
4. **Bytecode serialization + REPL** — after modules; format sketch in
   `runtime.md`.

## Known warts to clean up opportunistically

- top-level `var` aborts registration (should be a diagnostic or a feature)
- no shadowing diagnostics (`vscope_define` todo), no match exhaustiveness
- overlapping method names across impls: bare generic paths take the first
  registered impl
- `Point::new` vs `Point::<Int>::new`: expression paths require turbofish
- `EXPR_ASSOCIATED_CALL` is dead AST — remove or wire up
- codegen rejects *any* generic function, even uncalled ones, under `--run`
  (needs monomorphisation or boxed generics eventually)
