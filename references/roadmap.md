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
- **Aggregate runtime, data layer (milestone 5c-i)** — structs/enums/tuples
  as heap objects (`OBJ_STRUCT`/`OBJ_ENUM`/`OBJ_TUPLE`), field access
  (`.name` and tuple `.0`), full match compilation for
  literal/wildcard/bind/struct/tuple/variant patterns with guards and
  nested sub-patterns, tag-dispatch for enum variants, structural `==`, and
  `print` support for all three. `OP_MATCH_FAIL` makes a non-exhaustive
  match (unenforced by the checker) a runtime error instead of UB. Design:
  `runtime.md` "Codegen shapes" → "Match compilation". Methods, closures,
  and `?` are deferred to 5c-ii/5c-iii (below).

  Also fixed three pre-existing bugs surfaced by exercising the feature —
  none previously reachable since nothing exercised struct/enum/tuple
  patterns end-to-end before this milestone:
  - `check_struct_pattern`/`check_variant_pattern`/the tuple-pattern case in
    `check_pattern` all did `sub_pattern_error |= check_pattern(...)` —
    `check_pattern` returns `true` on *success*, so this treated success as
    failure, silently poisoning the type of any match arm whose pattern had
    an inner sub-pattern (a bind, nested struct, etc.) with no diagnostic to
    explain why. Fixed to `|= !check_pattern(...)`.
  - `EXPR_VARIANT`'s field-checking loop matched `init->fields[i]` against
    `variant_def->fields[i]` purely positionally, ignoring the field's own
    `ident` — for a struct-like variant (`Circle { radius }`), initializing
    fields out of source order would type-check but silently write values
    to the wrong field at runtime. Fixed to look up by name/index via
    `find_variant_field`, matching how struct-init already worked.
  - a bare qualified path to a unit variant or unit struct (`Status::Off`,
    with no `()` or `{}`) was rejected by `EXPR_PATH` resolution
    ("unexpected multi-segment path expression without a call") even though
    the rewrite machinery to construct it already existed
    (`rewrite_tuple_variant_call`'s `EXPR_PATH` branch was dead code before
    this fix). `Status::Off` now resolves like any other variant
    constructor.
- **Aggregate runtime, methods + `?` (milestone 5c-ii)** — impl methods
  compile like any other function (`self` is just a `ParamDef` with
  `is_self` set); `obj.method(args)` and bare associated/self-explicit calls
  (`Point::new(...)`, `Shape::area(s)`) both work. Methods and top-level
  functions share one `OP_GET_GLOBAL` slot space (`Module.methods`, slots
  continuing past `fun_count`) rather than adding a new opcode. `?` never
  reconstructs `Err` — it's the same enum instance regardless of the `Ok`
  payload type, since generics are erased at runtime — it just reads the
  tag and either extracts `Ok`'s field or `OP_RETURN`s the instance
  unchanged. Deliberately did *not* wire up the dead `EXPR_ASSOCIATED_CALL`
  node for the bare-call case (see the wart below); caching the resolved
  `FunDef` directly on the `EXPR_PATH` node was less invasive and didn't
  need a new type-checking path. Design: `runtime.md` "Methods" /
  "Propagate (`?`)". Closures are deferred to 5c-iii (below).

  Also fixed a real GC-rooting bug this surfaced: `heap_collect`'s
  constant-pool root scan only ever walked `module->funs[]`, never
  `module->methods[]` — a string constant interned inside a method's chunk
  (e.g. an interpolation text segment) was invisible to the GC and could be
  freed out from under a still-running method, corrupting the next read of
  it. Unreachable before methods had chunks of their own to hold constants.
  Fixed by having `heap_collect` also mark `module->methods[]`' constants.

- **Aggregate runtime, closures + upvalues (milestone 5c-iii)** — closure
  expressions (`|x| => body`) compile to their own chunk against a child
  `Cg` chained to the enclosing function, so name resolution turns captures
  into an upvalue list (`cg_resolve_upvalue`, de-duped by `cg_add_upvalue`,
  threaded through every intervening closure for multi-level capture). A
  capturing closure is a heap `ObjClosure { fun, upvalues[] }`; each captured
  cell is an `ObjUpvalue` that starts *open* (pointing at the live stack slot,
  so the variable and its closures share one value, mutation included) and is
  *closed* onto the heap (`close_upvalues`) when the slot dies — at
  `OP_RETURN` (function scope) and via `OP_CLOSE_UPVALUE` emitted by
  `cg_close_scope` at block/loop/match/`break`/`continue` pop points. Non-
  capturing functions stay plain `VAL_FUN`; `OP_CALL` handles both. GC roots
  the open-upvalue list and each closure `FunDef`'s constant pool (nested
  closures are collected into `module->closures`). Design: `runtime.md`
  "Closures & upvalues". A captured `for` variable is one shared cell (reads
  its final value) — a deliberate simplification, documented there.

- **Trait signatures + conformance (milestone 6a)** — trait item signatures
  now resolve (`resolve_trait_decl`): each `TraitMethodDef` gets a
  `method_type` built against the abstract trait `Self` (self is param 0), and
  `Self.Assoc` in a trait signature stays abstract (a `ty_assoc` projection —
  `TYNODE_ASSOC` special-cases a `TY_TRAIT` base). `tc_check_impl_conformance`
  then checks each trait impl: all associated types and all required methods
  present (methods with a default body may be omitted), and each method's
  signature matches after `trait_project` rewrites the trait signature into the
  impl's terms (`Self` → impl self type, `Self.Assoc` → the impl's concrete
  bound) and compares with `types_equal`. Design: `architecture.md` sema
  pass 3. Deliberately checker-only: default method *bodies* aren't checked,
  *calling* a defaulted-but-omitted method isn't dispatched yet, and extra
  inherent methods in a trait impl are tolerated — all noted as gaps.

- **Trait bounds (milestone 6b)** — type parameters now carry real bounds.
  `resolve_generic_param` merges inline `<T: A + B>` bounds with matching
  `where T: C` predicates (`resolve_bound_refs` resolves each ref through the
  type scope, rejecting non-traits) into the `TY_GENERIC`'s `bounds`, replacing
  the six "inline bounds are not supported" diagnostics. Bounds are enforced
  once inference settles: `infer_open_generics` stashes the source `TY_GENERIC`
  in each fresh unknown's `.bound`, and `infer_check_bounds` (run after
  `infer_finalize`) asks `impl_index_implements` whether the solved type has a
  matching `impl Trait for T`, reporting at the call site. Design:
  `architecture.md` "Trait bounds". Known gaps: explicit turbofish args bypass
  the unknown so they're unchecked, and `tc_check_impl` doesn't finalize
  inference so impl method bodies aren't checked either.

- **Cleanup pass (post-6b)** — paid down frictions surfaced by 6a/6b:
  - `ty_generic` now interns like every other `ty_*` constructor (bound order
    canonicalised *before* the probe — `type_intern` used to sort *after*
    hashing, filing entries under a slot no lookup would recompute). Generic
    identity is pointer equality again, matching the documented invariant.
  - That collapse exposed a real bug: `type_is_bare_generic_self` used pointer
    identity to tell a bare path (`Point::new`) from a method receiver, so
    `self.m()` inside `impl<T> Box<T>` selected *any* impl defining `m` —
    including `impl Box<Int>`'s. `impl_index_method` now takes an explicit
    `bare_path` flag; receivers pass false
    (`tests/fail/generic_self_method_leak.dt`).
  - `tc_check_impl` now runs `infer_finalize` + `infer_check_bounds`; unsolved
    unknowns and bound violations inside impl method bodies were silently
    accepted before (`tests/fail/bound_in_impl_method.dt`).
  - `type_sprintf` printed `fun(Int): ` — it never emitted the return type at
    all. Now `fun(Int) -> Bool`. Its `n += snprintf(...)` accumulation was also
    an out-of-bounds write for any type rendering past the caller's `char[64]`
    (snprintf returns the *would-be* length, underflowing `buf_size - n`);
    every step is clamped through `sp_bump`.
  - `resolve_bound_refs` reports "too many trait bounds" instead of silently
    dropping past `MAX_BOUNDS`, matching the parser's caps.

- **Trait completion, part 3 (milestone 6c)** — an abstract receiver now
  dispatches through trait signatures instead of the impl index:
  `resolve_bound_method_call` picks the first bound declaring the name (a
  bounded `T`, or the `Self` of a default body, which offers its own trait) and
  `check_trait_method_call` checks the call against that `TraitMethodDef`,
  projecting the trait's terms into the caller's. The same checker serves
  *inherited* default methods: when `impl_index_method` misses,
  `impl_index_default_method` finds an applicable trait impl whose trait
  declares the name with a default body. `tc_check_trait` type-checks default
  bodies once against the abstract `Self` (so `self.other()` inside one
  dispatches through the trait). `T.Assoc` on a bounded type parameter now
  resolves (replacing the "not yet supported" diagnostic), and explicit
  turbofish arguments finally get their bounds enforced, via
  `InferCtx.explicit_bounds`. Design: `architecture.md` "Calls through a trait
  bound" / "Associated-type projections".

  Making projections actually work required three fixes to the type layer,
  each a latent bug the moment a `T.Assoc` could reach inference at all:
  - `TY_ASSOC` wasn't interned, breaking the pointer-identity invariant every
    other structural type keeps: the `T.Unit` written in an annotation and the
    one `trait_project` produced were different pointers, so unifying them
    reached `infer_unify`'s `default:` and *aborted the compiler* on an assert.
    Now interned, with `TY_TRAIT`/`TY_ASSOC` handled in `infer_unify` as a
    plain mismatch rather than an assert.
  - `subst_apply` and `infer_apply` both ignored `TY_ASSOC`, so a projection
    never followed its base: `T.Unit` stayed `T.Unit` after T was solved.
    `subst_apply` now rewrites the base, `infer_apply` collapses the projection
    to the impl's binding once the base is concrete, and `infer_unify`
    normalises `TY_ASSOC` operands up front.
  - `type_sprintf`/`type_name_sprintf` printed a `TY_ASSOC` by reading
    `base->as.generic.name` unconditionally — garbage (a wild `StringView`) for
    the `Self.Assoc` case, which is exactly what a trait-signature diagnostic
    prints. Both print the base recursively now.

  Also factored the "does this impl apply to this receiver" logic, duplicated
  three times, into `impl_applies`. Known gaps: an inherited default method
  type-checks but can't run (`--run` reports it — the body needs
  monomorphising against the concrete self), and a call through a bound is
  unreachable at runtime anyway since codegen rejects generic functions.

- **Module system (milestone 7)** — a program is now the root `.dt` file plus
  everything reachable through `use`. No `mod` keyword was added: `use` both
  names the file and imports the items, so `use geo::point::{Point, Line};`
  loads `<root_dir>/geo/point.dt`. Discovery had to fuse with parsing (a
  module's dependencies *are* its `use` decls, which need an AST), so
  `compiler_phase_discover` is a worklist that parses a module, runs
  `mod_collect_imports`, and continues into whatever that appended;
  `compiler_phase_parse` is gone. `compiler_phase_dep_graph` is a real
  tri-colour DFS producing a post-order `topo_order`, with a back edge
  reported as a cycle naming every module in the chain. `pub` finally means
  something: `tc_link_imports` reads it off the `Decl`. Design:
  `architecture.md` "Modules".

  Three judgement calls worth recording:
  - **Linking is not where the old stub said.** The commented-out
    `mod_link_imports` sat in the *register* phase, which cannot work: module
    names are only defined into `m->tscope`/`m->vscope` during *resolve*, so a
    dependency has no exports until it is resolved — while the importer's own
    signatures need its imports already in scope. The only solution is per
    module, in topological order, immediately before `tc_resolve_module`,
    which is also why cycle detection is a prerequisite and not a nicety.
    Renamed `tc_link_imports` and moved to `src/sema.c` accordingly.
  - **Path handling is lexical, not `realpath`.** The build is `-std=c23`,
    which defines `__STRICT_ANSI__` and hides `realpath`/`getcwd`/`PATH_MAX`;
    an implicit declaration is a *hard error* in C23, not a warning. Module
    paths are a fixed base dir plus identifier segments, so lexical
    normalisation dedups them exactly — and unlike `realpath` it works for
    files that don't exist, which is what lets the missing-module diagnostic
    name the path it looked for.
  - **Linking scans the dependency's AST for the item, then copies the entry
    out of its scopes.** Scanning the decls is what puts `Decl.is_pub` in
    reach, and it is also what prevents accidental re-export: a dependency's
    scopes hold *its* imports and the builtins too, so a bare `tscope_lookup`
    would have given `use b::X` the meaning of `pub use`. The alternative —
    a visibility flag on every `VarEntry`/`TypeEntry` — would have touched 30+
    define sites.

  Also cleaned up while here, each a latent bug rather than a new one:
  - `modreg_add`'s duplicate check was a `// todo` and its only caller wrapped
    it in `assert(modreg_add(...))` — an assert with a side effect, which
    `BUILD=release` (`-DNDEBUG`) would have compiled away entirely, silently
    registering nothing.
  - `vscope_define`/`tscope_define` still don't detect duplicates and both
    lookups return the *first* match, so an import colliding with a local
    declaration would have resolved backwards — the import silently winning.
    `tc_link_imports` does its own conflict check; the `std` no-op case has to
    short-circuit before it, or `use std::io::print;` would report a collision
    with the builtin it names.
  - codegen matched the `print` builtin by name, so `use std::io::print as p;`
    type-checked and then failed codegen. `cg_names_builtin_print` resolves the
    alias through the module's std imports.
  - `resolve_bound_refs` claimed "modules aren't implemented, so a qualified
    path resolves by its last segment"; a qualified trait bound is now an error
    pointing at `use`, matching what `resolve_path` already did for types.

- **Runtime module linking (milestone 8)** — a multi-module program now runs.
  Every slot operand is one byte, so it cannot mean "index into my own
  module": `exe_link` (`src/codegen.c`) flattens every module's funs, impl
  methods, structs and enums into one program-wide `Executable`
  (`include/object.h`) and writes each definition's index back into its
  `slot`, so codegen only ever emits `def->slot`. `Module.methods`/`closures`
  and `Heap.module` are gone — the heap roots off `exe->globals`/`closures`,
  and `vm_run` takes the `Executable` instead of a `Module`. Design:
  `runtime.md` "Linking".

  The ordering constraints are the whole design:
  - **Linking is a separate step, before codegen, not something codegen does
    on the way past.** Compiling module A can emit a slot for a definition in
    module B, so B's numbering must be final first; the alternative is a
    patch-up pass over already-emitted bytecode.
  - **The heap must root off the linked tables, not the module being
    compiled.** Codegen interns string literals, so a collection can happen
    with most chunks still empty. A `FunDef` in the tables with a NULL chunk
    is a root with nothing to mark — harmless; one *missing* from the tables
    is not a root at all, and its already-interned constants get swept. This
    is the same class of bug as the 5c-ii one where `heap_collect` never
    walked `module->methods[]`, one level up.
  - Codegen still runs per module so each diagnostic is reported against its
    own source (`tests/fail_run/mod_unsupported`).

  One checker change was required, and it fixed a latent hole rather than
  adding a feature: codegen resolved a bare `foo()` by searching the enclosing
  module's `funs[]` by name, which finds neither an imported function nor an
  alias (`use lib::helper as h`). Single-segment `EXPR_PATH` resolution now
  records the `FunDef` it picked in `ExprPath.resolved_fun` — the same field
  `resolve_callee` already filled for qualified paths — and codegen reads that
  instead of searching. The name search survives only as the guard that
  decides whether a local `print` shadows the builtin.

  Also: `scripts/run_tests.sh` never globbed `tests/run/*/main.dt`, so a
  multi-file test could only ever be a compile-only one.

- **Hygiene diagnostics (post-8)** — four warts that each let a wrong program
  through silently, or crashed on one:
  - **top-level `var`** hit `tc_register_module`'s `default: assert(false)` —
    an abort, not a diagnostic. Now reported ("move it into a function"), and
    `DECL_VAR`/`DECL_POISON` are explicit cases in all three decl switches so
    the compiler cannot be aborted by a parse the checker forgot about.
  - **duplicate top-level names** were silently accepted: neither
    `vscope_define` nor `tscope_define` detects a collision and both lookups
    return the *first* match, so a redeclaration lost to the original with no
    diagnostic. `tc_check_duplicate_decls` runs before registration; the name
    scan it needs already existed inside `mod_find_own_decl`, so both now
    share `decl_item_name`.
  - **match exhaustiveness** is enforced (`check_match_exhaustive`) —
    Maranget's usefulness algorithm over a pattern matrix, tri-state so an
    unsolved column type reports nothing rather than guessing. Design:
    `architecture.md` "Match exhaustiveness". `OP_MATCH_FAIL` stays: a guarded
    arm can't count towards coverage, so falling past every arm is still
    reachable at runtime.
  - **`EXPR_ASSOCIATED_CALL`** and its `ExprAssocCall` payload are gone; the
    parser never produced either (see 5c-ii).

  Writing the exhaustiveness checker turned up the design trap worth recording:
  the obvious column type for a variant payload is the `FieldDef.type` off the
  definition, which is the *generic* `T`. Reading it there makes a correct
  `Opt::Some(true) | Opt::Some(false) | Opt::None` look like a gap, because `T`
  has no enumerable domain. The instantiated type is already on the pattern —
  `check_pattern` stamps `resolved_type` after substitution — so the matrix
  reads it from the first row that constrains the column.

- **Bytecode serialization (milestone 9a)** — `--emit-bc <out> file.dt` writes
  the linked program as a flat little-endian image; `--run <image>` decodes and
  executes it with no compiler in the process (the two forms are told apart by
  the magic bytes, not the extension). Format and rationale: `runtime.md`
  "Serialization".

  The design falls out of one observation: **an image is the runtime projection
  of the program, not a snapshot of the compiler.** Only the fields `vm.c` and
  `value_print` actually read off a `FunDef`/`StructDef`/`EnumDef` are written,
  so a loaded def has no types, spans or `Module` behind it — which is what
  makes the loader independent of the AST entirely.

  Both ordering constraints are the linking ones from milestone 8, one level
  over:
  - the string table precedes every record that indexes it, so writing is two
    walks (collect, then write) with an after-the-fact check that the table
    didn't grow during the second;
  - every definition is allocated before any record is decoded, so a `VAL_FUN`
    constant can name a function further down the file without a patch-up pass
    — and so `bc_load` can `heap_init` off complete tables before decoding the
    first constant pool, since interning a string constant is exactly the
    "collection mid-load" hazard codegen already has.

  `compiler_execute` split into `compiler_codegen` (link + codegen + find
  `main`) and the two things that consume it. Testing is a round-trip rather
  than a fixture: `scripts/run_tests.sh` emits an image for every `tests/run`
  program and re-runs it, so anything the format forgets is an output diff (17
  extra cases, all of them also verified under `--gc-stress`), plus two
  malformed-image rejections.

- **Monomorphisation (milestone 10)** — generic code runs. Every generic
  function, method and impl is compiled once per distinct tuple of type
  arguments; `Mono` (`include/codegen.h`) memoises `(FunDef, type args)` and
  doubles as the worklist. Design: `runtime.md` "Monomorphisation".

  The design falls out of one observation, the mirror of the serialization
  one: **the runtime is uniform in type arguments.** Field slots are
  declaration order, tags are per enum, no opcode reads a static type — which
  is exactly why `?` propagates an `Err` without rebuilding it. So a type
  argument changes one thing about the compiled code: *which function a call
  resolves to*. Monomorphisation duplicates code and nothing else; struct and
  enum defs stay shared across every instantiation, and the whole feature is
  "give a call site the right slot".

  Three ordering constraints, all of them the milestone-8 linking ones one
  level over:
  - **The type arguments have to be recorded by the checker, and cannot be
    read until inference is done.** The `Subst` `resolve_callee` /
    `resolve_method_call_expr` solve is the answer, but at the moment it is
    built its arguments are the call's fresh unknowns. So `cctx_record_inst`
    stashes a *copy* on the call node and queues it, and `cctx_solve_insts`
    rewrites it after `infer_finalize` — the same collect-then-write shape the
    bytecode string table needed. `Subst` moved from `sema.h` to `ast.h` for
    this: AST nodes store one now.
  - **A recorded type argument is in the enclosing definition's terms, not the
    program's.** `f::<T>()` inside a generic `f` records `T`. Instantiating it
    means pushing the *caller's* bindings through it (`Cg.subst`), which is
    what makes the queue a fixpoint rather than a one-level expansion —
    `describe_twice<Point>` → `describe<Point>` → `Point::show`.
  - **A generic definition gets no slot at link time,** so `exe->globals` is
    sized to the whole operand space and instances are appended during
    codegen. The consequence worth having: an uncalled generic is no longer an
    error, because nothing ever looks at it.

  The interesting call shape is dispatch through a bound. The checker resolved
  `v.show()` against the trait *signature*, so the node carries no
  `MethodDef` — only `bound_trait`/`bound_self`. Codegen substitutes the
  receiver into a concrete type and re-runs `impl_index_method`. That is the
  one place codegen consults the impl index, and it is the reason generics
  can't simply be erased.

  Also fixed, each a latent bug this made reachable:
  - `infer_unify`, `subst_apply` and `infer_apply` all ignored `TY_TUPLE` —
    the first *aborted the compiler* on its `default:` assert, the other two
    silently left `(T, T)` unsubstituted. Nothing had unified a tuple against
    a generic before; `first((1, 2))` does.
  - `impl_index_implements` judged a `TY_GENERIC` by searching the impl index,
    which has no entry for a type parameter — so a bounded generic could not
    hand its parameter to another bounded generic. It now answers from the
    parameter's own declared bounds, which is sound because the bound is
    re-checked where the outer function is instantiated
    (`tests/fail/bound_forward_missing.dt`).
  - `print` named as a value compiled to `OP_GET_GLOBAL 0` — a silently wrong
    function, since a builtin has no slot. Now a diagnostic.
  - a generic `main` hit `vm_run`'s "entry function was not compiled" assert.

  `MONO_MAX_DEPTH` bounds a divergent instantiation chain (`grow([v])` inside
  `grow<T>`). The slot ceiling would stop it too, but only after interning a
  few hundred types into a fixed-size table — so the limit lives where the
  divergence is.

## Next (in recommended order)

Estimates are relative to one focused session ≈ the checker-completion
milestone (~900 lines).

1. **Runtime destructuring** — `var (a, b) = pair;` and struct patterns in a
   binding position type-check but codegen rejects them, which is now the
   largest remaining hole in `tests/pass/in_fixed.dt` under `--run`. The match
   compiler already lowers every pattern shape against an `Accessor`; a
   binding is that machinery with no test and no arms.
2. **Inherited default method bodies** — the last construct in `in_fixed.dt`
   the VM refuses. `TraitMethodDef.default_impl` is never built, so there is
   no `FunDef` to instantiate, and `Self` in a trait body is a `TY_TRAIT`
   rather than a `TY_GENERIC` — a name-keyed `Subst` cannot bind it, so this
   needs `Self` to become a real type parameter of the default body.

Not on the roadmap: the **REPL** is a side feature, not a milestone — it lives
on the `feature/repl` branch (`--repl`, incremental compilation over one module
via `Module.decl_base`) and is not part of the main line.

## Known warts to clean up opportunistically

- no shadowing diagnostics for `var` (`vscope_define` todo); top-level item
  names do collide, but a `var` may silently shadow one in the same scope
- a bare unit variant of a generic enum can't be instantiated without a
  turbofish (`Opt::<Bool>::None`) — neither an annotation nor the argument
  position infers it
- overlapping method names across impls: bare generic paths take the first
  registered impl
- `Point::new` vs `Point::<Int>::new`: expression paths require turbofish
- every instantiation takes a global slot, so the 256-function ceiling is now
  reached by *use* of generics, not just by how many are written
- a generic definition's diagnostics are only seen where it is instantiated,
  so an unsupported construct inside an uncalled generic goes unreported
- trait impls are program-global: an impl applies even if its module was never
  imported (`ImplIndex` lives on the `TypeChecker`, not the `Module`)
- no `pub use` re-export, no glob `use a::*`, no module-qualified paths
- `pub` on impls/methods/fields is parsed and ignored — visibility is
  per-item at module granularity only
- module dedup is lexical, so one file reached by two different spellings
  (a symlink, say) would load twice and collide
- the linked slot spaces are one byte wide, so 256 functions/structs/enums is
  a hard program-wide ceiling (reported, not silently truncated)
- a bytecode image is structurally validated (bounds, indices, counts) but the
  code itself is not verified: an image with a plausible header and nonsense
  instructions can still crash the VM
- an image carries no source, so a runtime error in one reports function names
  with no line information
