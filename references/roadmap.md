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

- **Runtime destructuring (milestone 11)** — `var (a, b) = pair;` runs, and
  so does every other irrefutable pattern. The change is a deletion: a `var`
  binding *is* a `Pattern` now, so the parallel `BindingPat` grammar (parser),
  its 140-line type-checking switch (sema) and its dump (ast) are gone, and
  the three consumers each reuse the match machinery they already had.
  Design: `architecture.md` "Binding patterns", `runtime.md` "Destructuring a
  `var`".

  The observation the milestone turns on: **a binding is a match with one arm
  and no guard.** Everything follows from taking that literally rather than
  approximately.
  - Nesting, renaming field patterns (`Point { x: px }`), `_`, and the
    tuple-struct constructor spelling `Pair(a, b)` all arrive for free —
    the old `BindingPat` supported none of them, being a flat list of names.
  - "Irrefutable" needs no new analysis: it is *exhaustive* over a one-row
    matrix, so `check_binding_pattern` asks `matrix_covers` the same question
    `check_match_exhaustive` asks, and inherits its tri-state answer.
  - Because that answer is tri-state, codegen still emits the test pass and
    still traps through `OP_MATCH_FAIL`. An irrefutable pattern compiles no
    tests at all, so the backstop is free — the same role `OP_MATCH_FAIL`
    already plays for guarded arms.

  Deleting a checker path is only safe if the one that replaces it is at
  least as strict, and it was not: `check_struct_pattern` never compared the
  struct its path resolved to against the struct the value actually has, so
  `var B { x } = a;` (and `B { x } => ...` in a match arm, unreachable for
  `var` before this milestone but always live there) read A's field through
  B's declared types — `x` bound as `String` over an `Int`, and `x + "!"`
  printed garbage. `check_variant_pattern` had made the matching check for
  enums since 5c-i; the struct half now does too
  (`tests/fail/struct_pattern_wrong_type.dt`). The old `BindingPat` code had
  its own version of this check, which is what made the omission visible.

  Also fixed a pre-existing bug this surfaced, one the milestone made
  reachable from a second direction: `check_pattern` rewrites a `PAT_VARIANT`
  whose path names a struct into a `PAT_STRUCT` (and the mirror) with
  `*pattern = new`, which replaced the whole node and so dropped
  `resolved_type`. Exhaustiveness reads the column type off exactly that
  field and treats NULL as "unconstrained", so **every tuple-struct pattern
  looked like a coverage gap** — and since the struct spelling `Pair { a, b }`
  is rejected outright, a tuple struct could not be matched at all
  (`tests/run/match.dt`). Both rewrites now carry the type across.

- **Inherited default method bodies (milestone 12)** — the last construct
  `tests/pass/in_fixed.dt` reached that the VM refused, so the showcase now
  compiles *and* runs (it prints nothing, so it stays a `tests/pass` entry;
  `tests/run/trait_default.dt` is what exercises the feature). Design:
  `architecture.md` "Trait default bodies", `runtime.md` "Monomorphisation".

  The observation the milestone turns on: **a default body is a generic
  function whose first type parameter is `Self`.**

      trait Show { fun twice(self) -> Int { self.show() + self.show() } }
      ⇒            fun twice<Self: Show>(self: Self) -> Int { ... }

  What blocked it was that a `Subst` is keyed by *name*, and the trait's
  `Self` was a `TY_TRAIT` — nothing a substitution could bind, so there was
  no way to say "this copy, at this receiver type". Projecting the signature
  onto a real `ty_generic("Self", bounds=[trait])` once, in
  `resolve_trait_default_impl`, gives the body a `FunDef` like any other and
  the rest is machinery that already existed: `tc_check_trait` checks the
  body with `Self` in scope as that parameter, so `self.other()` is an
  ordinary call through a bound; `check_trait_method_call` records `Self`
  in the call's `inst` next to the method's own type arguments; codegen
  instantiates it exactly as it does a `<T: Show>` function. The trait's
  `method_type` keeps the abstract `Self`, because conformance and call
  sites are still checked against the trait, not against the copy.

  Net: ~120 lines added, no new concept in the backend — the diff is mostly
  one constructor and the two call paths (`resolved_default` on a concrete
  receiver, the `impl_index_default_method` fallback under a bound) learning
  to name a definition they previously had to refuse.

  Also fixed a pre-existing bug found while writing the tests, unrelated to
  traits but on the same code path: a closure capturing `self` (in *any*
  method) hit `compile_expr`'s `assert(cg->self_slot >= 0)` and **aborted
  the compiler**, because the parser leaves the receiver parameter nameless
  so `cg_resolve_upvalue` had nothing to find. `compile_fun_body` now
  registers it as a local called `"self"` — a keyword, so no user local can
  collide — and `EXPR_SELF` falls back to an upvalue
  (`tests/run/closures.dt`).

- **Trait objects (milestone 13)** — `dyn Trait` values dispatch dynamically,
  so one collection can hold several concrete types. Design:
  `architecture.md` "Trait objects", `runtime.md` "Trait objects",
  `language.md` "Trait objects".

  The observation the milestone turns on is the **dual of monomorphisation's**.
  Milestone 10 rested on "the runtime is uniform in type arguments, so a type
  argument changes exactly one thing: which function a call resolves to." A
  trait object is the case where that choice cannot be made at compile time —
  so it is *carried by the value*. A `dyn Shape` is the pair `(value, the
  table of slots monomorphisation would have picked)`. The vtable is the
  monomorphisation key moved from compile time into the value, and nothing
  else about the representation changes.

  Three things follow, and they are the whole milestone:
  - **`TY_DYN` is a separate kind from `TY_TRAIT`, and is *concrete*.**
    `TY_TRAIT` is the abstract `Self` of a default body — resolved statically,
    monomorphised. `TY_DYN` is a real runtime representation, so
    `type_is_concrete` says yes and a `dyn Show` keys an instantiation exactly
    like a struct. Sharing one kind would have put both dispatch strategies
    behind one type. The *checker* barely notices the difference:
    `resolve_method_call_expr` hands a `TY_DYN` receiver to the same
    `resolve_bound_method_call` a bounded `T` uses, since a trait object
    offers exactly the one trait it names. Only codegen tells them apart.
  - **Coercion is the one new concept.** The language had no subtyping at all:
    identity is pointer equality, `infer_unify` only decomposes structurally.
    That is enough everywhere else, but `Sq` → `dyn Shape` is a real operation
    (allocate the fat value, attach the vtable), not a re-reading of the same
    bits — so it is modelled as exactly that: explicit, one-way, attempted
    only where a value flows into a *known* `dyn` position, never inside
    unification. The test is `impl_index_implements`, the same question a
    bound asks, which is the honest way to put it: a trait object is a bound
    whose witness travels with the value instead of being resolved. The
    coercion is recorded on the `Expr` node, the same collect-then-consume
    shape `inst` already needed, because at the moment it is discovered the
    type may still be an unsolved unknown.
  - **Dispatch reuses `OP_CALL`.** `OP_DYN_METHOD` pops the trait object and
    pushes the method's function *then* the unwrapped receiver, which leaves
    the stack in the shape `OP_CALL` already understands. So dynamic dispatch
    added no call machinery, no frame changes, no arity special case — the
    same move that let methods reuse `OP_GET_GLOBAL` in 5c-ii.

  Object safety is checked where `dyn Trait` is *written*, not at the trait
  declaration. That is not a shortcut: a trait is free to be
  static-dispatch-only, and `tests/run/trait_default.dt` leans on every rule
  the check enforces (`-> Self`, `other: Self`, `Self.Label`, a method with
  its own type parameter). Only naming a trait as a *type* asks for more.

  Coercion sites are call arguments, `return`, a `var` with a `dyn`
  annotation, a struct/variant field initializer, and an element of a `[dyn T]`
  or `(dyn T, ..)` literal. The two literal cases needed the *hint* threaded
  through first — an array literal used to let its first element decide the
  element type, and a tuple literal ignored the hint entirely.

  Also fixed two pre-existing bugs this surfaced, neither about traits:
  - **`infer_unify`'s arguments were swapped at four call sites**, so those
    diagnostics printed backwards: `pair(1, "no")` on a `fun pair<T>(a: T,
    b: T)` reported "expected 'String' but got 'Int'" when `T` was already
    bound to `Int`. The convention is documented ("the *expected* type goes
    first") and every other caller honours it; the generic call-argument path,
    both generic method-argument paths, and struct/variant field init did not.
    Unification is symmetric about which side it binds, so only the wording
    was ever wrong — which is exactly why it survived.
  - a **parser bug**, found because a test wanted `return Rect { .. };` inside
    an `if`: `p->allow_struct_init` was cleared for the `if` *condition* and
    left cleared across `parse_block` for the body, so a struct literal was
    unparseable anywhere inside an `if` — while the identical code in a `for`
    body was fine, because `for` restores the flag before its block. The
    restriction only ever existed to make the `{` after a condition start the
    body (`tests/run/if_struct_literal.dt`). The `else` branch had a matching
    clear/restore pair around a line that only assigned NULL — dead code that
    made the omission look deliberate.

- **The standard library begins (milestone 14)** — `std::` names real modules
  now, and the first one is `std::cmp`. Design: `architecture.md` "The embedded
  standard library", `language.md` "The standard library".

  The observation that makes this a small milestone rather than a large one:
  **`impl Ord for Int` is legal.** A trait can be implemented for a primitive,
  so the standard library is written *in ducktape* rather than bolted on in C —
  which means milestone 13 finishing the trait machinery is what unlocked it,
  and `std::cmp` needed no new builtins, no new opcodes and no language change
  at all. The module was prototyped against the unmodified compiler first, and
  every line of it ran.

  `std::cmp` is `trait Ord { cmp; lt/gt/le/ge defaults }`, impls for `Int` and
  `Float`, and `max`/`min`/`clamp` over `T: Ord`. It is deliberately *not*
  object-safe (`other: Self`), which is the object-safety rule working as
  designed: it is a bound, and only `dyn` asks for more.

  The design decision worth recording is where the std/not-std distinction is
  allowed to live:
  - **A std module is an ordinary `Module` in the registry.** `use std::cmp`
    goes through the same `modreg_add`, the same dependency-graph edge, the
    same cycle detection, `pub` checking, linking and codegen as a user import.
    Making it special anywhere else would have meant re-deriving all of that.
  - **So the entire difference is one branch in `mod_parse`.** `Module.file_path`
    is only ever a registry key, a diagnostic label, and an argument to
    `read_file` — and only the third needs a real path. A `<std>/…` key reads
    from the embedded table instead. Pointing that branch at a directory is all
    it would take to make std filesystem-backed; nothing downstream can tell.
  - **The `.dt` files stay the source of truth.** `scripts/embed_std.sh` mirrors
    `std/*.dt` into `build/std_data.h` at build time, so std is editable and
    directly runnable, `make format` never sees generated C, and the test suite
    and `--emit-bc` images stay hermetic — no install path, no env var.

  `<std>/<name>.dt` is the key; the angle brackets are what make collision
  impossible, since a real module path is a base dir plus identifier segments
  and an identifier cannot contain `<`. The key ignores `base_dir`, so two
  modules importing std from different directories dedup to one entry
  (`tests/run/mod_std/`).

  `std::io` survives as the one `std::` path with no module behind it: `print`
  is registered into every scope by `tc_register_builtins`, so a real `std::io`
  exporting it would collide with the builtin already bound under that name.
  Every *other* unknown `std::` name became a diagnostic listing the embedded
  modules — it used to be a silent no-op that surfaced later, confusingly, as
  "undefined variable".

- **`std::option`, and the inference it needed (milestone 15)** — `Option<T>`
  is the second std module. Design: `language.md` "The standard library" →
  `std::option`, `architecture.md` "Types and inference".

  The module itself is unremarkable — a generic enum, a generic inherent impl,
  combinators taking closures, and `impl<T: Ord> Ord for Option<T>` — which is
  the point: nothing about `Option` is known to the compiler, it is not a
  prelude, and it needed no language change. It is also the first std module to
  `use` another (`std::cmp`), which the registry deduplicates against the
  program's own import of it.

  What blocked it was one wart: **`Option::None` could not infer its type
  argument**, so `return Option::None;` from a `-> Option<Int>` was a type
  error and only `Option::<Int>::None` worked. A standard type needing a
  turbofish everywhere is a bad advert, which is why the fix came first.

  It was two bugs wearing one hat, and separating them is the interesting part:
  - **The bare-path spelling never re-resolved.** `resolve_call_expr` and the
    struct-init path both rewrite a variant constructor and then *re-resolve*
    the rewritten node; the multi-segment `EXPR_PATH` case (`Status::Off`)
    rewrote and returned `r.type` — the enum's *declared* `Opt<T>` — so the
    abstract parameter escaped into the caller as if it were concrete, and the
    diagnostic read "expected 'Int' but got 'T'". Fixed by returning
    `resolve_expr(ctx, expr, hint)` like the other two, which routes it through
    the `EXPR_VARIANT` case that opens the parameters into fresh unknowns.
  - **A constructor with no fields has nothing to solve those unknowns from.**
    Every other generic constructor is solved by its arguments. `Opt::None`
    has none, so the unknown survives to whatever position the value flows
    into — solved there if that position unifies (an annotation, a `return`),
    but *not* if it compares with `types_equal`, which is what a call to a
    non-generic function does. `hint_type_args` seeds the type arguments from
    the expected type instead, so the constructor is already concrete by the
    time anything compares it. Seeding, not deciding: a hint that contradicts
    a field is still unified and still reported.

  The second half is the more general fix, and it applies to unit structs too
  (`var e: Empty<Int> = Empty;`) since the struct-init path grew the same
  seeding — the case milestone 14's unit-struct rewrite had left for the
  hint to handle "the same as `Wrap {}`", which turned out to be not at all.
  A bare unit variant with *no* expected type is now a "cannot infer type"
  error rather than a silently abstract type.

- **Native functions (milestone 16)** — a std function may have its body in C.
  `@native("io_print") pub fun print<T>(value: T);` is a bodyless declaration
  whose attribute names an entry in a registry `src/native.c` fills in. Design:
  `runtime.md` "Native functions", `language.md` "Native functions".

  The observation the milestone turns on: **the signature is the only part that
  has to be in ducktape.** Write it in the `.dt` file and the checker needs no
  special path at all — a native is an ordinary `FunDef` that happens to have a
  C body, so it obeys `pub`, imports by name or alias, takes an ordinary global
  slot, and is first-class for free (passable, storable, capturable, usable as
  a vtable entry) because `OP_GET_GLOBAL` does not care what is behind a slot.
  Compare the old builtin, which was a hand-built `FunDef`, a dedicated opcode,
  an alias-chasing helper, and a diagnostic explaining why it could not be a
  value.

  Three things follow, and they are the whole milestone:
  - **The GC calling convention is the one real rule.** A native that allocates
    can trigger a collection, and the VM stack is the root set — so `OP_CALL`
    leaves the arguments on the stack across the call, hands the native a
    `Value *` into it, and only pops them once the result exists.
    `OP_MAKE_DYN` already did exactly this for `heap_dyn`. Getting it wrong
    reproduces the 5c-ii bug where `heap_collect` never walked
    `module->methods[]`: something reachable-looking the collector cannot see.
  - **A generic native is never monomorphised.** The runtime is uniform in type
    arguments, so one C body serves every `T`; `cg_call_target` returns the
    `FunDef` itself and `exe_slot_fun` gives it a slot in spite of
    `fun_is_generic`. `print<T>` is the entire case, and it is milestone 10's
    observation read from the other side.
  - **An image carries a native by name.** A C function pointer is not
    serialisable and an image has no compiler behind it, so a fun record grew a
    *body kind*: chunk, native-by-name, or none. `bc_load` re-binds each name
    against the running binary, which makes a mismatched native ABI an error at
    load rather than a wrong call later — so no registry hash in the header is
    needed after all. An `@intrinsic` never appears in an image at all.

  `@intrinsic` is the second tier: the call lowers to an opcode inline, no
  frame, no slot. It is what finally lets a program *name* `OP_LEN`
  (`std::array::len`), until now reachable only from the `for` desugaring. The
  price is that an opcode is not addressable, so unlike an `@native` it can
  only be called, never used as a value — the one thing the two tiers do not
  share.

  **`print` stopped being ambient.** It is `std::io::print`, imported like
  anything else; there is no prelude, and 48 test files gained a `use` line.
  That deleted `OP_PRINT`, `cg_names_builtin_print`, `tc_register_builtins`,
  the "using a builtin as a value" diagnostic, the `ModImport.is_std` flag and
  its three readers, and the `std::io` no-op in
  `mod_collect_imports`/`tc_link_imports`. Two diagnostics got better for free:
  naming `print` as a value is now the ordinary "cannot infer type for 'T'"
  every generic function value gets, and an unresolved single-segment path says
  "cannot find 'x' in this scope" instead of "unknown type 'x' in path" —
  which, now that a missing `use std::io::print;` is a thing that happens, is
  the message most programs will meet first.

  **The design's own falsification test failed, and that is worth recording.**
  `runtime.md` predicted the milestone would be net-*negative* in compiler
  lines and said "if it is not net-negative, the design is wrong". It came out
  +193 across existing files (335 added, 142 removed) plus 187 new lines of
  registry. The design was not wrong; the *prediction* counted the deletions
  and forgot that three new surfaces come with them — an attribute grammar in
  the parser (+79), a serialization tier (+56), and the registry itself. What
  the deletions actually bought is not size but *uniformity*: `print` stopped
  being a case in five unrelated switches. The lesson is that "deletes five
  special cases" and "is smaller" are different claims, and only the first was
  ever the point.

  New std modules: `std::io` (`print`), `std::array` (`len`, the intrinsic),
  `std::string` (`len`, `slice`). `slice` allocates, so it is what exercises
  the calling convention, and it is also the first std function that can
  *fail*: a native sets `ctx->error` and the VM raises it at the call site —
  the closest thing to a panic until there is one.

- **`std::result`, and a real panic (milestone 17)** — `Option::unwrap` exists.
  Design: `language.md` "`std::panic`" / "`std::result`", `runtime.md` "Native
  functions".

  The observation the milestone turns on: **the panic was already built; only
  its type was missing.** A native could already fail — `ctx->error` becomes a
  runtime error at the call site, and `string::slice` had used it since
  milestone 16. What no ducktape function could do is *call* that and still
  type-check, because an accessor returning `T` has to produce a `T` on every
  path. `TY_NEVER` was exactly the answer and had existed since the checker
  learned about `return`; it simply had no name a signature could write.

  So the language change is four lines in `TYNODE_NAMED` — `Never` alongside
  `Int`/`Bool`/`Unit` — plus one registry entry. `infer_unify` already let
  `TY_NEVER` stand in for any type, so `return panic(msg)` satisfies any return
  type with no new rule, no coercion node, and no VM change whatsoever. The
  three roadmap questions answered themselves once put that way: a ducktape
  function *can* raise one (it is an ordinary call), the message is a `String`
  the caller builds, and the unwinding story is that there is none.

  `std::result` is then unremarkable, which is the point: `Result<T, E>` is an
  ordinary generic enum that happens to match the shape `?` has recognised
  structurally since 5c-ii, so propagation worked before the module existed and
  the module is only the combinators plus the two accessors a panic unlocks.
  `Option` gained the same `unwrap`/`expect` pair, and its file no longer opens
  by explaining why it can't have them.

  The one limit worth recording is where the design stops: a panic message
  cannot *name* the value that caused it. `"{e}"` on a generic `E` is "cannot
  interpolate a value of type 'E'" — printing an arbitrary value needs a
  `to_string`, which needs the formatting story the `Float` wart has been
  waiting on. `unwrap`'s message is therefore fixed and `expect` takes one from
  the caller, which is a smaller loss than it looks: the caller knows what it
  expected, and the frame list is printed either way.

  Also fixed a pre-existing bug this surfaced, and a nasty one — a
  **use-after-free at teardown**. `compiler_destroy` called `arena_destroy`
  *first*, then `diag_destroy`, which walks the diag array to free each
  message: both the array and the messages live in that arena. Every phase
  clears the bag at the *top* of its loop rather than the bottom, so the last
  module's diagnostics survive into teardown — meaning any failing compile was
  reading freed memory, and had been for as long as the arena has existed. It
  only faulted once enough modules were loaded to lay the arena out unluckily,
  which is why a single-file failure never showed it and
  `tests/fail/interp_generic.dt` has to `use` a std module to reproduce.

- **A formatting story (milestone 18)** — a type can decide how it renders.
  `std::fmt::Display` is the fourth std module and the first thing in the
  standard library the *compiler* knows by name. Design: `architecture.md`
  "Interpolation and `Display`", `language.md` "`std::fmt`".

  The observation the milestone turns on: **once a trait is what decides how a
  value renders, `"{v}"` is not a formatting feature at all — it is the call
  `v.to_string()`.** So that is what `check_interpol_seg` rewrites a
  non-primitive segment into: an `EXPR_METHOD_CALL` node, resolved by the
  ordinary machinery.

  Everything follows from taking that literally rather than approximately, and
  it is the same move milestone 11 made when a `var` binding became a one-arm
  match:
  - Dispatch through a `T: Display` bound, through a `dyn Display` vtable, an
    inherited default body, and monomorphising the instance all arrive
    *already working* — every one of them is a method-call shape the checker
    and codegen already had. The showcase (`tests/run/fmt.dt`) exercises all
    three and none of them cost a line.
  - **Codegen, `OP_INTERP`, the VM and the image format did not change.** The
    segment now evaluates to a String, and `stringify` has always passed a
    String through untouched. The diff is one std file, one checker helper, and
    a native for `Float` precision.
  - The primitives keep their built-in path, which is why no existing test
    needed an import: `"{1}"` still emits no call. The four impls in `std::fmt`
    exist for the *other* direction — so a `T: Display` generic can instantiate
    at a primitive — and each is written by interpolating `self`, which makes
    them free.

  Two details are what make the rewrite safe rather than merely short. The
  receiver has to be resolved *first* (to know whether it is a primitive), so
  the rewrite cannot re-enter `resolve_method_call_expr` — doing so would
  re-report the receiver's diagnostics and re-queue its instantiations, so that
  function was split into `resolve_method_call_typed` plus a three-line wrapper.
  And the `Display` check runs *before* the rewrite rather than letting method
  resolution fail, which is what keeps the diagnostic about interpolation
  (naming the bound to add, for a type parameter) instead of about a missing
  method — and what stops an unrelated inherent `to_string` from silently
  qualifying.

  **The design decision worth recording is that `Display` is a lang item, and
  that this is a real cost.** Every other std name is anonymous to the
  compiler — `Option` is an ordinary enum, `Ord` an ordinary trait, and
  milestone 14's whole claim was that the std/not-std distinction lives in one
  branch of `mod_parse`. Interpolation cannot be written that way: it has to
  decide *which* `to_string` a segment means, and leaving that to whatever is
  in scope would make the meaning of `"{v}"` depend on imports. So
  `TypeChecker.display_trait` is captured in `tc_register_trait`, keyed on the
  module (`mod_is_std(m, "fmt")`) and not just the spelling, so a user trait
  called `Display` stays an ordinary trait. It is one field and one branch, but
  it is the first crack in "the compiler knows nothing about std" and the
  honest thing is to say so rather than to pretend the branch in `mod_parse` is
  still the only one.

  **`print` is deliberately not held to `Display`** and still renders any value
  structurally through `value_print`. The two are different questions — "show
  me what this is" is a debugging view the runtime can always answer, while
  "render this for a reader" is the type's own decision — so holding `print` to
  the trait would have cost 48 test files an impl to buy uniformity that isn't
  real. That leaves the language with two renderings, which is the same split
  Rust draws as `Debug`/`Display`, arrived at from the other end.

  Also cleared: `std::fmt::float(value, precision)` is the first rendering
  control a `Float` has ever had, and the `-Woverlength-strings` warning
  `std/result.dt` started emitting in milestone 17 (a std module crossing the
  4095-character literal ISO requires support for) is suppressed in the
  Makefile with a note, so the build is warning-free again.

- **Module-granular impls and `pub use` (milestone 19)** — an `impl` applies
  where its module is reachable, and two implementations of one trait for one
  type may no longer be silently visible at once. Design: `architecture.md`
  "Where an `impl` applies", `runtime.md` "Monomorphisation", `language.md`
  "Modules".

  The observation the milestone turns on: **an `impl` has no name, so it cannot
  travel through `use` one item at a time — reachability is the only handle a
  module has on it.** Everything follows from taking that as the rule rather
  than as an excuse to keep the index global:
  - The visible set is a **union, not a filter**: `Module.visible_impls` is the
    module's own impls plus each dependency's *visible* set, deduped by pointer
    and built in topological order alongside `tc_link_imports`. So every
    `impl_index_*` lookup kept the signature it had — the diff is the argument
    changing from `&tc->impl_index` to a context field, in eleven places.
  - It is **transitive**, because `pub use` re-exports a type whose impls live
    one module further away. Which also means the *root* module still sees
    every impl in the program: the rule only bites between siblings, which is
    exactly where it should.
  - **Codegen needs the requesting module's set, not the defining module's.**
    `std::cmp::max<T: Ord>` needs `impl Ord for P`, written where `max` is
    called; `std::cmp` cannot see it. So `Instance.impls` travels beside
    `Instance.subst` — a bound's witness and its type argument are both written
    at the call site, and milestone 10's "push the caller's bindings through"
    is the same move one field over. Transitivity is what makes it sound: a
    request that starts in M only reaches modules M reaches, so M's set is a
    superset of every set below it.

  **Coherence replaces first-registration-wins.** Visibility alone does not fix
  the wart it was promoted for: a user's `impl Ord for Int` and `std::cmp`'s are
  *both* visible the moment `std::cmp` is imported, so scoping only changes
  which arbitrary winner is picked. `impl_defs_conflict` asks selection's own
  question — same trait, and either impl's `impl_applies` accepts the other's
  self type — and a conflicting pair is now an error at the two places a pair
  can first meet: a module's own impl meeting an imported one
  (`resolve_impl_decl`), and one `use` making two dependencies' impls visible
  at once (`tc_import_impls`). Both spans lie in the module being compiled,
  which is not a detail: diagnostics are printed against that module's source,
  so the span of the *offending impl* would have pointed into the wrong file.

  **The feature is only usable because the failure explains itself.** Making
  impls module-granular turns a program that used to work into "no method named
  'show' found for type 'S'" — technically true and completely useless. So
  `TypeChecker.all_impls` survives as the one program-wide list, *never selected
  from*, consulted only by `find_unimported_impl` to append "an applicable impl
  exists in module 'owner.dt', but this module does not import it" to three
  diagnostics: missing method, unsatisfied bound, and non-`Display`
  interpolation. That note is most of what makes the milestone land.

  `pub use` is then almost free, and that is the second observation: **the
  re-export was one lookup away the whole time.** `tc_link_imports` already
  scanned the dependency's decls rather than its scopes — precisely so that a
  plain `use` would *not* re-export — so adding `mod_find_use_alias` beside
  `mod_find_own_decl` and reading `Decl.is_pub` off the `DECL_USE` node is the
  entire feature. The parser change is moving `pub` in front of the `use`
  branch. The two failures get different wordings, because they are different
  mistakes: "is private in module 'a'" hides something that exists, while "is
  imported by module 'b' but not re-exported" was never offered.

  Deliberately not done: glob imports and module-qualified paths, both still
  listed as gaps. Neither is needed to make imports compose.

- **`Display` for the containers (milestone 20)** — `"{v}"` renders an array, a
  tuple, an `Option` and a `Result`. Design: `language.md` "`std::fmt`" → the
  containers, `architecture.md` "An impl's own bounds, at selection".

  The observation the milestone turns on: **the std code needed no compiler
  change at all — all four impls ran against the unmodified binary.** What the
  milestone actually is, is the check that makes them *safe to ship*.

  Milestone 19 supplied the permission. An impl used to be program-global, so
  shipping one would have claimed the rendering of `[T]` for every program in
  the language whether it imported `std::fmt` or not, and a user writing their
  own would have silently lost. Module-granular impls plus coherence turn both
  into ordinary outcomes: you get the impl if you reach the module, and writing
  a competing one is an error rather than a coin toss.

  What was still missing was the bound. `impl_applies` only ever asked the
  *structural* question, so an impl's own `T: Display` was never checked when
  the impl was selected — only felt inside its body. Shipping a container impl
  is what makes that unlivable rather than merely untidy:

      var ws = [Widget { id: 1 }];   # Widget has no Display
      print("{ws}");

  used to type-check and then fail at *codegen*, with a diagnostic naming a
  method call the user never wrote, inside `<std>/fmt.dt`. So bounds are now
  checked where the impl is selected, and the failure lands on the
  interpolation, naming `Widget`.

  Three things follow, and they are the whole compiler diff:
  - **The check is opt-in per call site** (`impl_applies`'s `bounds_idx`, NULL
    for structural-only). The three selection paths pass the visible set;
    coherence passes NULL *deliberately* — two impls for one type overlap
    whether or not their bounds are disjoint, and the conservative answer is
    the one coherence wants — and associated-type lookup passes NULL because,
    like coherence, it runs while the index is still filling.
  - **Answering recurses**, since `[[Int]]` asks whether `[Int]` is `Display`,
    selecting the same impl one level down. The type shrinks each step, so the
    only non-terminating shape is a self-referential blanket impl
    (`impl<T: Foo> Foo for T`); `IMPL_BOUND_MAX_DEPTH` is where that lives,
    for the same reason `MONO_MAX_DEPTH` does — at the divergence.
  - **The failure has to explain itself**, which is milestone 19's lesson read
    a second time. "`[Widget]` does not implement `Display`" is true and
    useless: the impl exists and the fix is one level down. So
    `note_blocking_bound` joins `find_unimported_impl` on the same three
    diagnostics, appending "an impl exists, but it requires 'Widget: Display'".

  **Placement follows a rule the library already set**: an impl belongs beside
  the type it is for. An array and a tuple are the two types with no module of
  their own, so those two impls live in `std::fmt`; `Option`'s and `Result`'s
  live in `std::option` and `std::result`, exactly where `impl<T: Ord> Ord for
  Option<T>` already was. Since milestone 19 that is not a filing decision but
  a visibility one — it decides who sees them.

  The rendering decisions, which are the ones the roadmap flagged as genuinely
  new: separators match what `print` already does (`[1, 2, 3]`, `(1, hi)`), and
  a nested `String` renders **bare**, because `Display` answers "render this for
  a reader" and quoting is a debugging affordance `print` keeps. Arity is
  per-impl — a tuple's length is part of its type and nothing is generic over
  it — so only `(A, B)` ships.

  Also cleared: `type_sprintf` rendered every array as `[...]` (a standing
  `todo`). Harmless while `[T]` never appeared in a diagnostic; not harmless
  now that "cannot interpolate a value of type `[Widget]`" is a message people
  will meet, and unreadable for the `[T]` case where the note names the bound
  to add.

- **Reachability-based linking (milestone 21)** — a definition costs a slot
  when something *reaches* it, not when it is written. Design: `runtime.md`
  "Linking" → "Reachability", `overview.md` pipeline steps 6–7.

  The observation the milestone turns on: **the monomorphisation queue was
  already the reachability walk.** Milestone 10 built `Mono` because a generic
  definition has no single body — so it is compiled once per *request*, and one
  nobody calls costs nothing and is not an error. Every non-generic definition
  around it was linked and compiled unconditionally. Reading the queue the other
  way round makes that the anomaly rather than the norm: a non-generic
  definition is an instantiation with nothing to specialise, so it arrives the
  same way. `mono_reach` is `mono_request` with an empty key and no copy,
  `mono_seed(main)` is the root, and `codegen_module` — the pass over the
  registry — is gone.

  So this is a deletion, not a mechanism. `exe_link` no longer hands out a
  single slot; it sizes the tables and fixes the variant tags, and the whole
  two-pass walk over `reg->modules` collapses. `exe_slot_fun`'s three rules
  (an intrinsic gets no slot, a native gets one even when generic, a generic
  gets none) move into `cg_call_target`, which already had to state all three
  from the other side — they were duplicated, and now they are said once.

  Three things follow, and they are the whole diff:
  - **`slot != SLOT_NONE` is the memo**, so no second table is needed and a
    recursive call terminates for exactly the reason a recursive *generic*
    already did: the slot is handed out before the body that would ask again is
    compiled. Which is also the GC ordering constraint from milestone 8 read
    again — a `FunDef` in the tables with a NULL chunk is a root with nothing to
    mark, but one missing from the tables is not a root at all.
  - **A non-generic body uses its own module's impl set, not the requester's.**
    That is the one place `mono_reach` differs from `mono_request`, and it is
    milestone 19's rule stated precisely: a bound's witness travels with a type
    argument because the argument is chosen at the call site, but a non-generic
    body has no bound to witness and selects entirely from where it was written.
    So who reaches it first cannot change what it compiles to — closing, for
    non-generics, the half of the "whichever requested it first decides the
    body" wart that visibility alone could not.
  - **Structs and enums follow the same rule, at the constructor.** Neither is
    reached by being declared nor by being matched — a pattern tests a tag and
    reads fields by index, and neither operand names the definition. A variant
    *tag*, though, is not a slot: it is an index within its own enum, so it
    stays eager, which is the one thing `exe_link` still does over the registry.

  Measured on the test suite: `tests/run/std_result.dt` went from 24 globals to
  16, `std_option.dt` from 24 to 17, `fmt_containers.dt` from 25 to 19 (images
  11–15% smaller); `std_cmp.dt` is unchanged at 16, because it uses everything
  it imports. `tests/run/unused_defs/` declares 260 functions across two modules
  and links because it calls three — on master it failed with "the program has
  261 functions and methods".

  **The price is a real one and has its own test.** A body nothing reaches is
  never handed to codegen, so a construct the VM refuses inside one goes
  unreported (`tests/run/unreachable_body.dt` names an `@intrinsic` as a value
  and runs, purely because nothing calls it). That was already true of every
  generic definition; it is now true uniformly. Only *codegen* is demand-driven
  — the checker still sees every function in every module, so a compile-only
  run is unaffected, which is what keeps the trade acceptable.

  Net +12 lines across `src/` and `include/` (253 added, 241 removed), most of
  it prose — which, after milestone 16's lesson about predicting diff size, is
  worth stating as an observation rather than a claim: an eager pass and a
  demand-driven one are close to the same size, and what changed is which one
  the rest of the compiler has to agree with.

  Also cleared: `TraitDef.slot` and `ImplDef.slot` were dead fields — neither a
  trait nor an impl was ever addressable — and `FUN_SLOT_NONE` is now
  `SLOT_NONE`, since it is the initial state of four `slot` fields rather than
  a fact about functions.

  Worth recording as a limit found while testing: the struct and enum slot
  spaces cannot actually be *reached* today. `TYPE_INTERN_CAP` is 256 with a
  70% load assert, so a program cannot name more than ~179 distinct types at
  all — the intern table binds long before 256 structs do. The on-demand rule
  is right for them, but the pressure it relieves is entirely in `globals[]`.
  (Milestone 22 lifted both, and `tests/run/many_structs.dt` is the program
  that reaches past 256 struct slots.)

- **The ceilings (milestone 22)** — no fixed limit aborts the compiler any
  more. Design: `runtime.md` "Bytecode" (the operand rule) and "Linking" →
  "Reachability", `architecture.md` "Types and inference", `overview.md`
  pipeline steps 6–7.

  The roadmap listed this as "wider slot spaces", which is half of it. Four
  fixed limits stood in the way of a merely *large* program, and three of them
  failed by **aborting** rather than diagnosing — a worse failure than the one
  the roadmap named:

  | limit | before | after |
  |---|---|---|
  | globals / structs / enums / vtables | 256, diagnosed | u16 operand, tables grow |
  | constants in one chunk | 256, `assert` | u16 operand, diagnosed |
  | distinct types (intern table) | ~179, `assert` at 70% load | grows and rehashes |
  | statements in one block | 256, `assert` | grows |

  The observation the milestone turns on: **an operand is one byte when it
  indexes a *frame*, two when it indexes a table the *program* grows.** Which
  is not a budget but a structural fact — the VM reserves `STACK_HEADROOM`
  (256) slots for a single frame, and `CG_MAX_LOCALS` is 256, so a u8 local or
  upvalue index *is* the frame size. Widening it would address stack that
  cannot exist. Everything on the other side of that line — globals, structs,
  enums, vtables, constants — indexes a table with no such backing bound, so
  each gets two bytes and `SLOT_MAX` (65536) replaces four scattered 256s.

  Stating the rule that way settles the two questions the roadmap left open:
  - **A two-byte operand, not a wide-operand opcode pair.** The pair saves a
    byte in the common case at the cost of two opcodes per slot space and a
    decode branch on the hottest ops. There is nothing to choose *between* here
    — no operand ever wants to be narrow — so the uniform width is the honest
    encoding.
  - **A declaration-bounded index stays u8** — a field, a variant tag, a vtable
    method. These look program-wide but are not: the parser already caps and
    diagnoses each of those counts well below 256, so widening them would buy
    a ceiling nothing can reach.

  Three things follow, and they are most of the diff:
  - **The tables now grow instead of being pre-sized**, which pre-sizing to the
    operand space made unavoidable: 256 pointers apiece was free, 64K is not.
  - **So linking finished dissolving.** `exe_link` handed out slots until
    milestone 21 made them demand-driven and sized the four tables until they
    learned to grow; with both gone it no longer touches the `Executable` at
    all. What is left is `exe_assign_tags` — a variant tag is not a slot, and
    it is the one part of a program's layout that cannot wait for a reference
    that might not come. There is no longer a linking pass distinct from the
    walk itself.
  - **The image format is version 4.** Widening a slot operand changes how
    existing code bytes decode, so an old image has to be refused rather than
    misread — which is what the version field was always for. Entry index,
    per-chunk constant count and vtable method indices went to u32 for the same
    reason the operands went to u16: each now spans a table that can outgrow
    the width it was written at.

  **The intern table was the real ceiling, and it was lower than the one the
  roadmap named.** A program aborted at ~179 distinct types — reachable by
  merely declaring structs, and *below* the 256 slot space, which is why
  milestone 21 recorded that the struct and enum slot spaces "cannot actually
  be reached today". It is now open-addressed with a doubling growth path. The
  ordering constraint is the one `ty_generic` already had: growth rehashes, so
  it must run *before* a probe, because a slot index is only meaningful under
  the mask it was computed with — filing an entry under a mask no later lookup
  recomputes is exactly the bug milestone 6b's "sort after hashing" was.

  Also fixed, a latent use-after-free the growth path made worth closing: the
  intern table is a **process global** (it has to be — pointer identity means
  two `Type *` compare equal only if one table produced them) whose entries are
  compiler-arena memory, and now whose array is too. Nothing reset it, so a
  second `compiler_init` in one process would have read freed memory.
  Unreachable from `main`, which compiles once — but the `feature/repl` branch
  is precisely a second compile, so `type_intern_reset` runs in
  `compiler_destroy`, beside the `diag_destroy` ordering the milestone-17
  use-after-free established.

  Each of the four ceilings has a test that pins exactly *it*: on master
  `many_globals.dt` reports 257 functions, `many_structs.dt` aborts in
  `type_intern`, `many_constants.dt` in `chunk_add_const`, and
  `long_block.dt` in `parse_block`. Getting there took a second pass — the
  first drafts all had 300-statement `main`s, so all four died in the parser
  and only the last ceiling was really under test.

- **Growable arrays (milestone 23)** — `[T]` stops being fixed-size:
  `std::array::push` appends, `pop` removes, and the buffer underneath doubles.
  Design: `runtime.md` "Heap & GC" → "Growing an array", `language.md`
  "`std::io`, `std::array`, `std::string`".

  The observation the milestone turns on: **only two of the operations are
  things an array cannot express about itself** — a slot that did not exist
  before, and one fewer than there was. So those two are the whole C surface,
  and `pop`, `first`, `last`, `is_empty` and `clear` are ordinary ducktape on
  top. That is milestone 16's "the signature is the only part that has to be in
  ducktape" read from the other end, and it is what buys the API its shape:
  `pop` returns an `Option<T>`, which a native *cannot* do at all, since its
  contract is "n values in, one out" and it has no handle on the `VariantDef`
  an enum instance needs. The raw remove-last is therefore private, panics on
  empty, and exists only to be wrapped
  (`tests/fail/array_pop_last_private.dt`).

  **The language surface did not change.** No new type, no new syntax, no new
  opcode, no image change, and not one line in the checker — `push` is an
  ordinary generic native and `[T]` gained a field the language cannot name.
  The whole milestone is below the checker, which is the honest reason it is a
  small one: what the roadmap called "a real allocator question" turned out to
  be a real *GC* question, and the answer was already written down.

  The ordering rules are milestone 8's, one level down, and both directions
  matter:
  - **The new buffer is allocated before anything about the array changes**, so
    the collection that allocation may trigger still finds `count` live values
    in the old `items`. The array is rooted (for `push`, by being an argument
    still on the VM stack — milestone 16's calling convention, unchanged), so
    the collector *will* walk it; what it walks has to be the buffer that is
    actually there.
  - **`count` rises only once the slot exists.** Raising it first points the
    collector at a `Value` never written — the same class of bug as a `FunDef`
    missing from `exe->globals`, or `heap_collect` never walking
    `module->methods[]` in 5c-ii: something reachable-looking that the
    collector cannot read correctly. Popping is the mirror, and it makes the
    convention's second half visible for the first time: lowering `count` drops
    the value, which survives only because the VM pushes the result with
    nothing allocating in between.

  `free_obj` releasing `cap` rather than `count` is the one place the split
  leaks out of `object.c`; everything else about a `[T]` — `OP_ARRAY`,
  `OP_INDEX_GET`/`SET`, `OP_LEN`, the `for` desugaring, `value_print`,
  structural `==` — reads `count` and did not change.

  The cost worth recording is a **transitive one, and it is the design
  working rather than a wart**: `pop` returning `Option` makes `std::array`
  depend on `std::option`, and since milestone 19 impl visibility is transitive
  through `use`. So `use std::array::push;` now hands a program `std::fmt`'s
  and `std::cmp`'s impls as well, and with them coherence's refusal to let it
  write its own `impl Display for Int`. `std::array` used to be a leaf. The
  alternative was a panicking `pop`, which trades a worse API for a shallower
  graph — not a trade worth making, but worth saying out loud, because it is
  the first time a std module's *dependencies* are part of its public contract.

  **Also fixed a pre-existing bug, and this one was not surfaced by the
  feature** — it was found by running the suite under AddressSanitizer to check
  the new growth path, which is worth recording as its own lesson: the array
  work was clean, and the sweep found something else. **A closure inside a
  generic body was compiled into the `FunDef` on the AST node**, which is one
  node for every instantiation, so each instantiation overwrote the last one's
  chunk — and the earlier chunks' `VAL_FUN` constants went on naming it, so
  they built closures over somebody else's body. `mono_request` has cloned the
  enclosing `FunDef` per instantiation since milestone 10; `compile_closure`
  never did the same for the closure inside it.

  It read as a heap-buffer-overflow because the wrong body indexes fields by
  the wrong layout: a one-field `A` handed to a two-field `B`'s method reads
  `fields[1]` off the end of the struct. The tell is that
  `tests/run/trait_default.dt` has been *passing on the wrong answer* — its
  `bigger` default body compares two garbage reads, and the branch it happened
  to take was the right one. `tests/run/closure_generic.dt` pins it directly
  (on master it prints 49 for a value of 7), and the suite is now
  ASan-clean.

- **A buildable String (milestone 24)** — `std::string::builder()` returns a
  `StringBuf`: a growable text buffer that appends in place, so building a
  string stops being repeated `+`. Design: `runtime.md` "Heap & GC" → "Growing
  a string buffer", `language.md` "`std::io`, `std::array`, `std::string`",
  `architecture.md` "Types and inference".

  The observation the milestone turns on: **interning is what makes a `String`
  immutable.** Not a choice about strings — a consequence of the table. An
  `ObjString` is filed under the hash of its bytes and two equal strings are one
  pointer, which is exactly what makes `==` on strings a pointer compare; bytes
  that change cannot be in that table. Appending in place would either leave the
  object under a hash no lookup recomputes, or re-intern the whole accumulation
  at every step — which *is* the `+` cost being complained about. So a buffer
  cannot be an `ObjString` at any stage. It is the object deliberately kept out
  of the table, and `build` is the one-way door back in.

  Everything about the shape follows from that:
  - **`StringBuf` is a separate type, not a mutable `String`.** Making it a
    flavour of one would put both invariants behind a single type, the same
    mistake milestone 13 avoided by keeping `TY_DYN` apart from `TY_TRAIT`.
    The checker then knows nothing else about it: it is inert in the three
    switches it appears in, and its absence from `check_interpol_seg`'s
    primitive list is what makes `"{b}"` an ordinary "does not implement
    `Display`" rather than a special case.
  - **It is a builtin type whose operations live in std — the arrangement
    `String` already has.** So it is *not* another lang item in the sense
    milestone 18 recorded: `TypeChecker` still knows no std item, only a type
    name in `TYNODE_NAMED`, four lines beside `Never`.
  - **Four natives, and the rest is ducktape.** Existing, growing, its length,
    and becoming a String are what a buffer cannot express about itself;
    `join`, `concat` and `repeat` are written on top. Milestone 23's rule, and
    `repeat` is what shows the buffer earning its keep — it copies bytes
    straight in, allocating no String per copy, which an array of parts could
    not do.
  - **`std::string` stays a leaf**, which is the milestone-23 cost paid back
    the other way. `join` iterates with `for p in parts`, whose desugaring emits
    `OP_LEN` itself, so it needs no `use std::array::len` — and therefore hands
    a program none of the impls that import now drags in transitively.

  The ordering rules are milestone 23's, and the interesting part is that only
  half of them carries over. Allocate the new buffer before anything about the
  object changes, and keep the object rooted across it (free for `strbuf_push`:
  the buffer is `args[0]`, still on the VM stack) — unchanged. But "`len` rises
  only once the bytes exist" is *not* a GC rule here: bytes are pure payload,
  `mark_obj` does nothing for an `OBJ_STRBUF`, and an unwritten byte is garbage
  rather than a `Value` the collector will try to read. That asymmetry is what
  shows milestone 23's second rule was about the **collector**, not about the
  buffer.

  **The alternative considered was no new object kind at all**: a builder could
  be a `[String]` of parts with one `concat` native at the end, which also turns
  the quadratic `+` loop into a linear one and costs nothing anywhere in the
  compiler. What it cannot do is append something that is not already a String —
  every part has to be interned first, so `repeat("=", 40)` interns 40 strings
  to throw them away, and padding (the next roadmap item) would intern one per
  space. That is the line the new kind buys, and it is worth recording that it
  is a *narrow* one: the O(n²) half of the wart was fixable without any of this.

  Nothing else in the runtime moved. A `StringBuf` is never a chunk constant —
  no literal syntax, only a native makes one — so the image format is untouched,
  and `--emit-bc` round-trips `tests/run/strbuf.dt` with no serialization work
  at all.

- **An empty thing is still a thing (post-24 cleanup)** — the compiler is
  UBSan-clean, and getting there turned up a real bug. `make sanitize` rebuilds
  under ASan+UBSan and runs the suite.

  Every report was one idea: **a zero-length thing was being described by a NULL
  sentinel instead of by its count**, and `memcpy`/`memcmp`/`memset` require a
  valid pointer even for zero bytes. Three producers, and the fix belongs in a
  different place for each — which is the part worth recording, because the
  first two look identical from the report:
  - **An allocator hands out an address.** All three `Allocator` implementations
    answered a zero-size request with NULL, so `memcpy(xs, src, sizeof(T) * n)`
    — correct-looking code, over an `xs` allocated for exactly that many — was
    undefined for `n == 0`. Now a zero-size request returns a valid pointer that
    must not be dereferenced, which is also what `malloc` is permitted to do and
    what glibc does, and it keeps NULL meaning one thing: allocation failed. Ten
    call sites across `parser.c`/`sema.c`/`codegen.c` stopped being UB without
    being touched.
  - **A view points into something.** A `StringView` has no address of its own,
    so an empty one legitimately points nowhere (`(StringView){0}` is how an
    absent name is spelled). The fix there is at the *comparison*: `sv_equal`
    tests the length first, because there is no pointer to fix.
  - **A scratch buffer is valid when empty.** `ts_init` (sema's `TypeScratch`)
    special-cased zero to NULL; it now points at its own inline buffer, like
    every other length.

  **That last one was what exposed the real bug**, and it is the kind this
  project keeps finding: `infer_open_generics` decided whether a call supplied
  explicit type arguments by testing `concretes` for NULL, then indexed it up to
  the number of type *parameters* — two different counts. A call with no
  turbofish supplies zero arguments against a definition with several
  parameters, so the loop ran off the end of an empty array and read whatever
  followed it as a `Type *`. It was unreachable only because every empty
  argument list happened to be spelled NULL: correct by accident, not by
  construction. Both arrays now carry their own length and the guard is
  `i < concrete_count`, which is the same lesson as the allocator's one level
  up — *an empty array is described by its length, never by its pointer*.

  The symptom is worth recording too, because it argues for the sanitizer run
  more than the fix does: a wild `Type *` read out of a mapped library region,
  surfacing as an assert in `type_sprintf` (a switch that is exhaustive, so a
  garbage `kind` falls off the end) at a call site three phases away from the
  cause. It also **disappeared under ASan**, which is why both sanitizers run
  together now — ASan's own allocation layout hid a read that UBSan named
  exactly.

- **Ordered strings (milestone 25)** — `impl Ord for String`, so `max`, `min`,
  `clamp` and a sort work on text. Design: `language.md` "`std::cmp`",
  `runtime.md` "Heap & GC". One native, one impl, and two placement decisions
  that turned out to be the whole content of the milestone.

  The observation it turns on: **interning bought equality and nothing else.**
  Since 5b, `==` on a String has been a pointer compare, and that has read like
  a property of strings — it is really a property of the *table*, which
  guarantees one object per distinct byte string. Ordering asks the table a
  question it cannot answer: pointer order is allocation order, arbitrary and
  not even stable between a run and its `--emit-bc` replay. So the table hands
  `string_cmp` exactly one shortcut, the equal case, and the rest is a real walk
  over the bytes. The gift 5b made was narrower than it looked, and this is the
  first thing to stand outside it.

  **It has to be a native**, by milestone 23's rule — the two operations a `[T]`
  cannot express about itself — and String's version of that rule is sharper
  than the array's, because the circularity is visible: the finest handle
  ducktape has on a String's contents is `slice`, and comparing two one-byte
  slices would need string ordering, which is the thing being defined. A pure-
  ducktape `compare` is not slow, it is impossible.

  **Ordering is a trait, not an operator.** `<` and `>` stay numeric opcodes and
  `"a" < "b"` is still "comparison requires numeric types". That is not a gap
  left open: `Ord` is already how the language spells the comparison of anything
  that is not a number (`Money`, `Option<T>`), and teaching the operator about
  one more primitive would make `String` the exception rather than the rule.

  The decision worth recording is **where the impl lives**, because the two
  candidates are not symmetric and the asymmetry is milestone 19's transitive
  impl visibility, seen from the standard library's side for the first time.
  The trait is in `std::cmp` and the native belongs in `std::string` (that is
  where operations on `String` live), so *whichever module hosts the impl
  imports the other* — and since 19 that import is not free: it hands every
  dependent of the host whatever impls the imported module ships.

  - impl in `std::string` → `use std::string::len;` would also deliver `impl Ord
    for Int` and `Float`, and with them coherence's refusal to let a program
    write its own. `std::string` would stop being a leaf, undoing milestone 24's
    one deliberate promise.
  - impl in `std::cmp` → `std::cmp` imports a module of free functions that
    ships **no impls at all**, so the visible set grows by exactly the one impl
    being added.

  So: **an import's cost is measured in impls, not in code**, and a dependency
  should point at the impl-poor module. That is the same fact milestone 23 paid
  for in the other direction (`pop` returning `Option` made `std::array` reach
  `std::fmt` and `std::cmp`), stated as a rule rather than as a regret. It also
  matches a precedent already in the tree that nobody had had to justify:
  `impl Display for String` lives in `std::fmt`, not in `std::string`. **The
  impl goes with the trait.**

  Both directions were checked rather than assumed, and the probes are worth
  keeping in mind because coherence makes the obvious test misleading — a
  program cannot write a conflicting `impl Ord for Int` without importing `Ord`,
  which drags the impls in by itself. The observable question is *method
  dispatch without importing the trait*: `use std::string::compare;` then
  `3.lt(9)` is still "no method named 'lt'" (std::string reaches nothing), while
  `use std::cmp::max;` then `"a".lt("b")` works, and `use std::array::push;`
  then `3.lt(9)` works three hops out.

  Nothing in the compiler or the VM moved. `string_cmp` does not allocate, so
  it does not even exercise the native calling convention's rooting rule — the
  one thing `std::string`'s other natives exist to demonstrate.

- **A `Char` type (milestone 26)** — `'a'` is a value, and a program can finally
  get inside a String. Design: `language.md` "`std::char`, and what a `String`
  is made of", `runtime.md` "Values" / "Native functions", `architecture.md`
  Scanner / "Types and inference".

  The observation the milestone turns on: **a `String` is bytes, and a `Char` is
  not one.** Everything the runtime does with text has been byte-shaped —
  `len` counts bytes, `slice` cuts at byte offsets, milestone 25's `compare` is
  `memcmp` — and that was consistent only while nothing could look inside. A
  Char is the first thing that has to say what a String is *made of*, and the
  answer cannot be "a byte" without making the name a lie for text the language
  can already hold in a literal (`"héllo"` has been legal since 5b).

  So the two views stay apart, and everything follows from refusing to blur
  them:
  - **The bridge is a conversion, never an index.** `chars(s) -> [Char]` and
    `from_chars` cross over; there is deliberately no `char_at(s, i)`, because
    `i` would be a byte offset, a byte offset is not a character position, and
    that spelling would make confusing the two the *default* rather than the
    mistake. `len(s)` is 17 where `chars(s)` has 15 entries, and the API never
    lets those two numbers meet.
  - **`chars` can fail, and that is honest rather than untidy.** `slice` is
    indexed in bytes, so a program can halve a multi-byte sequence; the result
    is a String that is not valid UTF-8. Only a `Char` promises to be a
    character, so the promise is enforced where one is made — at `chars`, and
    at `from_code`, the only other way to conjure one.
  - **A Char is stored decoded.** `VAL_CHAR` is a `uint32_t` scalar value, so
    the encoding appears at exactly two edges: `utf8_encode` on the way out
    (`value_print`, the VM's `stringify`) and `utf8_decode` in the two natives
    that read a String. Strict both ways — overlong, surrogate, out of range all
    rejected — which is what lets every *other* path take for granted that a
    Char it holds can be written.

  The second observation is milestone 23's rule, and it cuts deeper here than it
  did for arrays: **what a Char cannot express about itself is its number.** So
  `code`/`from_code` are the whole of `std::char`'s C surface, and `is_digit`,
  `is_alpha`, `to_upper`, `to_lower`, `to_digit` and `impl Ord for Char` are all
  ordinary ducktape — a code point *is* an Int, so every classification is a
  range test and every case conversion is an addition. Four natives total, two
  of them in `std::string` (`chars`, `push_char`), and `from_chars` is ducktape
  because `push_char` exists.

  **`push_char` is the milestone-24 wart paid off, and it explains that
  milestone in retrospect.** "A `StringBuf` can only be appended to from a
  `String`" was listed as a gap; the note beside it said byte- or number-level
  appends "are the difference that made the buffer worth a new object kind in
  the first place". This is the first one, and it is what makes `from_chars` a
  loop rather than a fifth native — the alternative would intern a one-character
  String per character, which is the exact cost the buffer exists to avoid.

  **The checker barely noticed.** Adding a *primitive* is four lines beside
  `Never` in `TYNODE_NAMED`, a case in `resolve_expr`, an entry in three inert
  type switches, and the name in `check_interpol_seg`'s primitive list — the one
  place it differs from `StringBuf`, since a Char renders itself. Nothing in
  sema had to learn what a character is. Struct fields, generics keyed on
  `TY_CHAR`, `dyn Display`, `Option<Char>`, patterns and closures all arrived
  working, with no case anywhere for any of them.

  The cost is instead in the three places `StringBuf` never had to go, and they
  are what made this the larger of the two open std items:
  - **Literal syntax.** `TOKEN_CHAR` is the one token whose *content* the
    scanner validates rather than merely delimits. Finding the end needs escape
    awareness anyway (`'\''` holds a quote), so a second reader would be a
    second copy of the same knowledge: `char_literal_value` decodes a lexeme and
    returns the message describing what is wrong, the scanner reports, and the
    parser re-reads a lexeme it knows is good. A payload field on `Token` was
    the alternative, and it would widen every token in the array to carry a
    value one kind has. The escape set is the string one with `\{` traded for
    `\'` — a character literal has no interpolation, and a quote is what closes
    it — plus `\u{…}`, which is the only way to write a character that cannot be
    typed.
  - **A `Value` representation**, which was free (a plain value, no heap object,
    no GC involvement, `==` on two `uint32_t`).
  - **A new constant kind in the image**, the first since the format was written
    in 9a. `BC_C_CHAR` is the only tag whose payload is *not* total over its
    bits — every other one can decode anything it reads — so `bc_load` validates
    it rather than trusting it. An image may not be the one place in the runtime
    where a Char is not a scalar value. Appending the tag rather than inserting
    it leaves the existing numbering alone, which costs nothing.

  **`impl Ord for Char` confirms milestone 25's placement rule rather than
  merely obeying it.** The impl goes with the trait, in `std::cmp`, so `std::cmp`
  imports `std::char` — and the reason that is the cheap direction is that
  `std::char` ships no impls, nor does the one module *it* imports
  (`std::panic`, for `to_digit`). "An import's cost is measured in impls, not in
  code" now has a second instance, and this one sharpens it: a dependency on an
  impl-free module is free *no matter how much of it is used*. The contrast with
  `String` is the interesting half — ordering a Char needs no native at all,
  because `code` hands the comparison two Ints and `<` on an Int is an opcode,
  whereas ordering a *string* of them was exactly the circularity that forced
  `string_cmp` into C.

  **The classifications are ASCII-only, and saying so is the design.**
  `is_alpha('é')` is false and `to_upper('é')` is unchanged. Full Unicode case
  mapping is a table, not a range test, and it cannot be approximated by one —
  so the honest move is to name the limit rather than ship something right for
  English and quietly wrong elsewhere. That leaves milestone 25's case-folding
  wart *narrowed* rather than cleared: the language can now get inside a String,
  which was the blocker, and what remains is a data problem rather than a
  language one.

  No pre-existing bug surfaced, which is worth recording because it breaks a run
  of them: the suite was green and `make sanitize` clean on the first try. The
  most plausible reason is that a Char adds a new *kind* rather than a new
  relationship — nothing about it makes an existing path reachable from a
  direction it was not already reachable from, which is what every one of the
  last several latent bugs turned out to need.

- **Object-safe traits with associated types (milestone 27)** —
  `dyn Iterator<Item = Int>` dispatches dynamically over a trait the
  object-safety rule used to reject outright. Design: `language.md` "Trait
  objects", `architecture.md` "Trait objects (`dyn Trait`)", `runtime.md`
  "Trait objects".

  The observation the milestone turns on: **`Self.Item` is not `Self`.** Object
  safety rejects `Self` outside the receiver because the coercion *erased* the
  concrete type — a `-> Self` or an `other: Self` asks the caller for something
  it can no longer have. A projection looks like the same problem and is not:
  it is not the erased type, it is a *function of* it, and a function of an
  erased thing can be pinned by writing down its result. So `dyn
  Iterator<Item = Int>` is not a trait object with decoration — it is the
  trait object plus the part of the signature the vtable erased, put back where
  the caller can read it.

  Three things follow, and they are the whole milestone:
  - **A `TY_DYN` is a trait plus the impl's associated-type table, written at
    the use site.** One entry per `trait->assoc_types`, in declaration order,
    so the array is *total* and its ordering canonical — which is what lets
    interning keep deciding identity by pointer, exactly as it does for a
    struct's type arguments. Filling it is therefore also the completeness
    check: a hole is a missing binding, a name matching no hole is a wrong one.
    Every associated type must be bound whether or not a method mentions it,
    because two `dyn Iterator`s that agree on the trait and nothing else are
    not one type.
  - **Nothing in the backend moved.** An associated type is erased at runtime
    exactly as a type argument is, so codegen, the vtable, both opcodes and the
    image format are untouched, and the vtable memo key stays `(trait, self)` —
    which still determines the bindings, since an impl binds each once. This is
    milestone 13's own observation read once more: the vtable is the
    monomorphisation key moved into the value, and a binding is a fact about
    the *type*, so it never had to travel.
  - **Coercion grew a second question, and one of them is a solve.**
    `impl_index_implements` asked whether the trait is implemented; now each
    binding is compared against what the impl actually bound. The mismatch is
    reported inside `check_coerce_dyn` rather than left to the caller, whose
    diagnostic could only say the two types differ and not that the trait *is*
    implemented and only the binding disagrees — the poison convention one
    level up: one mistake, one message. The exception is a binding that is
    still an unsolved unknown (`[dyn Iterator<Item = T>]`), where the impl is
    the only thing that knows; that case unifies, and it is the one place the
    coercion solves rather than checks.

  The binding list is deliberately **not** a type-argument list, which is why
  the path after `dyn` is parsed in a new `PATH_BARE` mode and the `<` is read
  by `parse_assoc_bindings` instead. Handing it to `parse_type_args` would
  report "expected '>'" at the `=`; more to the point, a trait's own type
  parameters are a different question, still unsupported, and `dyn Into<Int>`
  remains without meaning.

  A side effect worth recording, because it narrows a listed wart rather than
  adding one: **a projection through a trait object can key an instantiation.**
  `id(d.next())` compiles where the same call on a bounded `T` reports "type
  argument 'T' is not known here", because `trait_project` collapses
  `Self.Item` against the trait object's own binding, so what reaches codegen
  is already a concrete type. The wart survives for bounds, which is where it
  was always the harder half.

  A mistake made *inside* the milestone is worth recording next to the
  pre-existing ones: the first draft compared bindings with `types_equal`, on
  the stated theory that "a binding is written down on both sides, and nothing
  here is being solved". Inside a generic that is false, and the probe that
  found it (`first([Counter { n: 5 }])`) is now part of
  `tests/run/dyn_assoc.dt`.

  And one real pre-existing bug, found while checking what to write in the
  "Next" list rather than by the feature itself: **a generic trait aborted the
  compiler.** `impl Into<Int> for S` reaches `resolve_path` — a *bare* trait
  name never does, since the type scope answers it directly — where `TY_TRAIT`
  fell into `default: assert(false)`. So the one spelling that gets there was
  the one nothing handled. Two diagnostics now, because they are two different
  mistakes at two different places: declaring a trait's type parameters
  (`trait Into<T>`, where they are silently dropped — each is now bound as
  poison so the signature using one does not report again), and naming a trait
  with type arguments. The second returns *successfully* with a poison type
  rather than failing, so the caller's blanket "unresolved type" does not pile
  on top.

- **Generic traits (milestone 28)** — `trait Into<T>` carries its type
  arguments, so `impl Into<Int> for S`, `T: Into<Int>` and `dyn Into<Int>` all
  mean something. Design: `language.md` "Generic traits", `architecture.md`
  "Generic traits", `runtime.md` "Monomorphisation" / "Trait objects".

  The observation the milestone turns on: **a trait's type parameters are
  ordinary generic parameters of every signature it declares.** So `TY_TRAIT`
  stops being a name and becomes a *reference* — a `TraitDef` plus arguments,
  interned on both — and a reference is exactly what the three places a trait
  can be named already were: an impl head, a bound, a `dyn`. Everything that
  knew how to substitute a type parameter then carries them for free.
  Conformance, a call through a bound and a default body's instantiation each
  apply one `Subst` more (`trait_ref_subst`), and the backend never learns
  that traits changed at all: an argument is erased at runtime exactly as an
  associated type is, which is milestone 27's own observation read once more.

  Three things follow, and they are the whole milestone:
  - **The bound is where the arguments were always missing.** `TypeGeneric`
    held `TraitDef *`, so a bound could name a trait and nothing about it.
    Holding a `TY_TRAIT` instead makes `S: Into<Int>` a question with an
    answer, and — because trait references are interned — the check stayed the
    pointer comparison it already was. It also gives a bound somewhere to put
    a *type*, which is what makes `fun conv<T, U: Into<T>>` writable: bounds
    now resolve left to right, defining each parameter as they go, and
    `infer_open_generics` rewrites the stashed bound through the substitution
    it just built so `Into<T>` is checked as `Into<Int>` rather than as a
    literal no impl heads.
  - **An impl applies to a *pair*, not to a receiver.** `impl_applies` takes
    the trait reference alongside the self type and matches the head against
    both, so either half may pin the impl's own parameters, and coherence asks
    the same question from both sides — which is what lets one type implement
    one trait at several arguments (`Into<Int>` and `Into<Fahrenheit>` for one
    `Celsius`) without it being a conflict.
  - **That makes a bare method call the one thing the receiver cannot
    decide.** `c.into()` has two bodies, and a bound would have named the
    reference. So the expected type breaks the tie — `impl_index_method` grew
    a `ret_hint`, consulted only when there is more than one candidate, so a
    wrong hint still reports the ordinary mismatch rather than "no method".
    This is the one place the feature costs the language a rule rather than
    reusing one.

  `dyn` needed a small grammar change and no new concept: the bracket list now
  holds the trait's positional type arguments *then* its named associated-type
  bindings (`dyn Pipe<String, Out = Int>`), told apart by two tokens of
  lookahead. Object safety is untouched — a trait's type parameters are
  written down by whoever names the `dyn`, so unlike `Self` they were never
  erased, which is the same argument milestone 27 made for `Self.Item`.

  A trait argument may also be left to the impl to solve
  (`fun first<T>(xs: [dyn Into<T>])`), which is milestone 27's
  "the coercion solves rather than checks" exception one bracket list over.
  Unlike an associated-type binding it can be *ambiguous*, since a type may
  implement the trait at two arguments — reported rather than guessed
  (`tests/fail/trait_arg_ambiguous.dt`).

  No pre-existing bug surfaced, which is the second milestone in a row to be
  green on the first `make sanitize`. Two latent ones were *pre-empted* by the
  design rather than found by it: `subst_apply`'s "every generic must be in
  the substitution" assert is a real invariant everywhere it instantiates a
  definition and simply false when opening a bound, so it grew a `total` flag
  instead of being relaxed; and `Expr.coerce_dyn` had to join `inst` on the
  queue `cctx_solve_insts` drains, because a trait argument — unlike the
  `TraitDef *` it replaced — can still be an unsolved unknown when the
  coercion is discovered.

- **`std::convert`, and the call it needed (milestone 29)** — `From` and `Into`
  are one relation written from two ends, and a program writes only the first.
  Design: `language.md` "`std::convert`", `architecture.md` "Calls through a
  trait bound" / "An impl parameter the head does not pin", `runtime.md`
  "Monomorphisation".

  The observation the milestone turns on: **what a value converts into is not a
  fact about the value.** Every dispatch the language had rested on the
  opposite — a receiver names its impl, or a bound names the reference — and a
  conversion is exactly the case where neither can. So the two ends of the
  relation are two traits, and each supplies the half the other cannot:
  `From` is qualified by the type it *produces*, and `Into` is pinned by the
  type its result *flows into*.

  Three things follow, and they are the whole milestone:
  - **An associated function is a method with no receiver, so the path
    dispatches instead.** `impl<T, U: From<T>> Into<U> for T` needs
    `U::from(self)`, and a trait signature with no `self` has nothing to
    dispatch on — which is why `T::from(v)` reached `resolve_path`'s
    `default: assert(false)` and **aborted the compiler**. It is now the same
    dispatch a bounded receiver gets, with `bound_trait`/`bound_self` moved
    onto the `ExprPath`: the bounds are searched exactly as
    `resolve_bound_method_call` searches a receiver's, and codegen's two
    branches share `cg_bound_target`. A `self` parameter is not special there —
    the signature is taken whole, so `T::value(v)` passes the receiver as an
    ordinary first argument, which is the reading a method call is sugar for.
  - **An impl parameter the head cannot pin is pinned by the expected type.**
    `impl_applies` demanded every parameter be bound by the self type or the
    trait reference; the blanket's `U` is bound by neither. So
    `impl_index_method` splits the question — `impl_match_head`, then the
    candidate method's return type against `ret_hint`, then
    `impl_head_complete`. It stays a hole-filler rather than a selection rule:
    with the head already total the hint is milestone 28's tie-break unchanged,
    and what the hole is filled with is *verified* by the impl's own
    `U: From<T>` bound, not trusted. That is what makes `26.into()` and
    `'a'.into()` reach different `From` impls for one `Steps`.
  - **The blanket is what makes the pair honest, and it costs the language a
    rule.** Coherence is deliberately blind to an impl's bounds, so
    `impl<T, U: From<T>> Into<U> for T` overlaps every `Into` impl that could
    ever be written: importing `std::convert` means you write `From` and never
    `Into`. That is Rust's rule arrived at from the same direction. The
    reflexive `impl<T> From<T> for T` is absent for the same reason one step
    worse — it would leave a trait nobody could implement.

  Two pre-existing bugs surfaced, both latent since milestone 28 gave a trait
  type arguments and neither reachable before a blanket impl of a generic
  trait existed to reach them:
  - **an impl's own bound was checked unsubstituted.** `impl_bounds_satisfied`
    asked whether `Meters` implements a literal `From<T>` — the impl's own
    parameter, not the type the head had just pinned it to — so a bound
    naming another parameter of the same impl could never be satisfied. This
    is the check milestone 28 already fixed one level over, for a *function's*
    bounds (`infer_open_generics` rewriting the stashed bound); the impl half
    had no such shape to exercise it until now.
  - **a `Subst` is keyed by name, and a name had two live meanings.** For
    `impl<T: Tag> Boxed<Int> for T` against `trait Boxed<T>`, the trait's
    argument and the impl's parameter are both "T", and `cg_inst_key` read the
    call's recorded arguments first — so the impl parameter silently took the
    trait's value and the body compiled against the wrong type. Renaming the
    impl's parameter made it work, which is what identified it. An impl
    parameter now reads the impl match first, because nothing else can speak
    for it.

  Also cleared, because the module needs it: **a builtin type may qualify a
  path** (`Float::from(7)`). The struct path context carried only a `Type`, so
  it became `PATHRES_CTX_TYPE`, and the builtin names moved into one
  `type_named_builtin` shared with `TYNODE_NAMED` so the two spellings cannot
  drift. Without it an impl written for a primitive would have been reachable
  only through `into`.

  The limit worth recording is where selection still does not look: two `From`
  impls for one type are told apart by the *receiver* under `into`, and not at
  all under `Meters::from(7)`, which takes the first registered impl. Selection
  reads a self type, a trait reference and now a return type — never an
  argument.

- **Selection by argument type (milestone 30)** — a qualified associated call
  `Steps::from(v)` reads its argument to choose among several `From` impls for
  one type, closing the gap milestone 29 named on the line above. Design:
  `architecture.md` "Selection by argument type".

  The observation the milestone turns on: **the argument is to `from` what the
  receiver is to `into`.** Milestone 29 read the type a call *produces* to pin
  an impl parameter the head left open; this reads the type a call *consumes*. A
  conversion qualified by its output (`Steps::from`) names the produced type but
  not the source, so when `From` is implemented for `Steps` twice the argument
  is the only thing that can say which — and selection had never read one, so
  the first registered impl won and every other argument was a type error
  against it.

  The whole difficulty is ordering, and it is milestone 29's constraint one
  level over: a callee is resolved *before* its arguments, because the parameter
  types are the hint the arguments resolve against. Reading the arguments to
  pick the callee inverts that, so the ambiguous case is split out rather than
  folded in:
  - **path resolution only flags the ambiguity.** `assoc_candidate_count` in
    `PATHRES_CTX_TYPE` sets `overload_recv` when more than one impl declares the
    name for the self type, still returning the first match — all a value
    context, which has no arguments to offer, can use. `Point::new` is excluded:
    a bare generic self selects by method name, not by a trait argument.
  - **`resolve_callee` declines to commit**, handing the self type and name back
    through an `AssocOverload` out-param instead of building a callee type.
  - **`resolve_assoc_call` is the one place arguments precede selection.** It
    resolves them hint-free — the argument is precisely what selects the impl,
    so a value that needs the parameter as a hint cannot disambiguate anyway —
    and reuses those types both to select and to check, so nothing is resolved
    twice.

  The selector adds no new matching. Each candidate parameter is a pattern
  `impl_type_match` tests against the resolved argument — the same
  non-committing matcher the head uses against the self type — and a hole the
  arguments leave is filled by the return-type tie-break milestone 29 already
  built, after which `impl_head_complete` checks totality and bounds. A unique
  acceptor wins; none is a failure that names the argument types and lists what
  each candidate takes (which is most of what makes it fixable); more than one
  is the ambiguity coherence makes practically unreachable but the selector
  still refuses. As with the return-type case, only a method with no type
  parameters of its own takes part. The resolved node is then indistinguishable
  from any other associated call — a concrete `resolved_fun` and the impl's
  substitution as its instance — so codegen needs no new case and the image
  round-trip covers it for free. The milestone is entirely in the checker.

  The limit worth recording is that the argument must type on its own: it is
  resolved hint-free, so `Steps::from(Option::None)` cannot be disambiguated —
  the one value that would tell the impls apart is itself waiting on an impl to
  say what it is. That is not a gap selection can close, since the argument is
  the only evidence there is.

- **Trait-qualified calls (milestone 31)** — a call may name its trait in full,
  `Into::<Fahrenheit>::into(c)`, closing the receiver spelling milestone 30's
  note left unread. Design: `architecture.md` "Trait-qualified calls",
  `language.md` "The standard library" → `std::convert`.

  The observation the milestone turns on: **it is the exact dual of milestone
  30.** That read the *argument* type to pick among impls of one produced type,
  because the self type was known and the trait reference was not; this reads the
  type a call's *receiver* has, because the trait reference is written and the
  self type is not. And `(self type, trait reference)` is precisely the pair
  `impl_index_method` has selected on since generic traits (milestone 28), so
  the whole milestone is teaching the path grammar to reach that lookup — the
  resolved node is a concrete `resolved_fun`, indistinguishable from any other
  associated call, so **codegen needs no new case** (same as milestone 30) and
  the image round-trip covers it for free. Entirely in the checker, ~150 lines.

  The structure is milestone 30's split, one field over, since the ordering
  constraint is the same — the callee cannot be selected before its receiver is
  resolved:
  - **`resolve_path` only names the trait and method.** A `TY_TRAIT` segment
    that is not last stops erroring "cannot access member of a trait" and
    transitions into a new `PATHRES_CTX_TRAIT`; the final segment is looked up
    among the trait's methods and returned as `PATHRES_TRAIT_QUALIFIED`. No impl
    is chosen — the self type is not written here.
  - **`resolve_callee` declines to commit**, handing the trait reference and
    method back through the deferral out-param `AssocOverload` had become — now
    `CalleeDefer`, since it carries two deferral reasons.
  - **`resolve_trait_qualified_call` is the one place the receiver precedes
    selection.** It resolves the argument at the method's `self_index` hint-free
    (a receiver whose type is unknown cannot select — the dual of "the argument
    must type on its own"), selects via `impl_index_method(self, trait_ref)`,
    and checks the arguments against the resolved signature, the receiver among
    them positionally: a trait-qualified path is the spelling where a method's
    `self` is an ordinary first parameter, the same uniform reading `T::value(v)`
    already leaned on.

  No hint reaches the selector: the reference already names every trait
  argument, so there is no hole for milestone 29's return-type tie-break to fill
  — which is the whole advantage of writing it over a bare `c.into()`. A generic
  impl (the `std::convert` blanket) monomorphises exactly as `x.into()` does,
  since the resolved node is the same shape.

  Three things are refused, each with a diagnostic that names the alternative
  rather than the internals — the milestone-19 lesson that a feature is only
  usable because its failure explains itself:
  - an **associated function** (`from`, no `self`) has no receiver to select by,
    so the qualified `Type::from(..)` spelling is where a source type is written;
  - a method the impl **inherits from a default body** rather than defining needs
    the bound-dispatch machinery a receiver call has and this concrete path does
    not, so the diagnostic points at `x.method(..)`;
  - an **abstract receiver** (a bounded type parameter) has no concrete impl to
    resolve to, and `s.into()` already dispatches through its bound
    unambiguously, so the spelling adds nothing there.

  Also fixed a pre-existing bug this surfaced, and a compiler-aborting one: a
  generic trait whose method declares type parameters of its *own*
  (`trait Box<T> { fun wrap<U>(self, ..); }`) crashed `tc_check_impl_conformance`
  on an `assert`. The check substitutes the trait's type arguments into the
  trait signature to compare it against the impl's, and did so *totally* — but a
  method's own `U` is legitimately absent from that substitution (both signatures
  keep it under the same name), so `subst_apply`'s "every generic must be bound"
  assert fired. The same non-total application the type layer already uses for
  opening a bound is the fix; the comparison still catches a real mismatch
  because both sides retain `U` (`tests/run/generic_trait_method.dt`,
  `tests/fail/generic_trait_method_mismatch.dt`). Unreachable before, since no
  test had a generic trait method with its own type parameter — trait-qualified
  calls made one worth writing, and opening that `U` is exactly what lets the
  spelling reach such a method.

  Also corrected a stale doc row milestone 30 left behind: the "Not yet
  implemented" table still claimed `Meters::from(7)` could not select by
  argument, which milestone 30 had just made false. It now records the residual
  both spellings share — a qualified selection whose argument (or receiver) is
  itself unresolved.

- **Text operations (milestone 32)** — `std::text` is searching, splitting,
  trimming and parsing: `starts_with`, `ends_with`, `find`, `contains`, `split`,
  `trim`, `parse_int`. Design: `language.md` "The standard library" →
  `std::text`.

  The observation the milestone turns on: **these operations cannot live in
  `std::string`, and the reason is a dependency cycle, not taste.** Every one of
  them either answers with an `Option` or builds a `[String]`, so `std::string`
  would have to import `Option` or array `push`. But `std::cmp` imports
  `std::string` (for `compare`, since milestone 25) and `std::option` imports
  `std::cmp`, so that import closes the loop `string → option → cmp → string`,
  which the dependency graph rejects outright. `std::string` is a leaf *by
  necessity*: it is what `std::cmp` is built on, so it cannot reach back up to
  the `Option` these return. The operations that do live one module higher.
  This is `std::array` losing its leaf status once `pop` returned an `Option`,
  taken to the point where the split is forced rather than merely tidy — and it
  is why the milestone shipped no compiler change at all: `std::text` ran against
  the unmodified binary the moment it was written in the module the graph allows.

  Within the module the milestone-26 rule that `std::string` and `std::char`
  were built around decides the byte-vs-character fork the roadmap left open, and
  it answers it *both* ways because the two operations that expose a position and
  the two that inspect a character are different questions:
  - **a position is a byte offset**, because a position is one you will `slice`
    at — `find` and `split` are `slice`+`==` and never read a byte alone, so a
    `find` result feeds straight back into `slice`. The cost is that `find`
    slices and interns a candidate at every position (O(*n·m*) allocations): a
    String's only reader is `slice`, so a window has to be cut out to be
    compared, there being no `byte_at` (milestone 26 refused the spelling);
  - **a character is classified by its value**, so `trim` and `parse_int` cross
    to the `chars` view — whether something is a space or a digit is a fact about
    a character, not a byte, and an ASCII space is single-byte only by luck of
    the encoding.

  `starts_with`/`ends_with` need neither, so they are the pure `slice`+`==` ones.
  The empty pattern reads consistently — `starts_with(s, "")` and `find(s, "")`
  both succeed at 0, `split(s, "")` returns `s` whole (no position lacks `""`, so
  the finder would never terminate) — and `split` yields one more piece than
  separators, so an edge or doubled separator gives an empty piece. Known limits,
  each recorded as a wart: `find`'s per-position interning, and `parse_int`
  wrapping rather than detecting overflow (a widening multiply the language
  cannot spell).

- **Module-qualified paths (milestone 33)** — `use std::string;` binds the
  module as a qualifier, so `string::len(s)` is a real path. Design:
  `language.md` "Module-qualified paths", `architecture.md` "Module-qualified
  paths", `grammar.ebnf`.

  The observation the milestone turns on: **an item import and a module
  qualifier are the same reachability with two spellings.** A `use` already
  loads the file, adds the dependency edge, and imports the impls (milestone 19);
  a module import adds exactly one thing on top — *a name for the module* — so it
  costs no new runtime, no new impl-visibility rule, and no new resolution
  concept. The first segment of a path may now be a module, and everything after
  it resolves against that module's scope exactly as a top-level path resolves
  against the current one. It splits across the same three seams every module
  feature does:
  - **Discovery decides the shape.** A braced `use a::b::{X}` is always an item
    import; a bare `use a::b;` is ambiguous — item `b` of `a`, or module `a::b` —
    and only file existence tells them apart. So the parser stops stripping the
    trailing segment (it keeps the whole path and a `bare` flag) and
    `mod_collect_imports` resolves it: the full path names a module ⇒ module
    import, else the last segment is an item of the prefix. A std module is
    exactly `std::<leaf>`, so its split is by segment count, not a probe.
  - **Linking binds the name.** A module is neither a value nor a type, so it
    goes in neither scope: `Module.qual_modules` is its own `{name, target,
    span}` table, filled beside the item imports. The collision check is the
    reused `link_name_taken` plus a scan of the qualifiers already bound —
    `string` cannot be both a qualifier and a local.
  - **Resolution adds one state.** A first segment that is neither a type nor a
    builtin is looked up in `qual_modules` (reached through the new
    `TypeResolver.module`, which resolve, check and type-annotation contexts all
    carry). A hit enters `PATHRES_CTX_MODULE`, whose export lookup reuses the
    exact `is_pub` gate `link_import_item` applies — so a qualified path and an
    item import see the same set — and hands the entry to `pathres_step_entry`,
    the factored-out body of the scope state's type switch. `a::Point::new`
    reuses the struct→associated machinery; a qualified function resolves to the
    same `PATHRES_METHOD` a bare call does, so **codegen never learns a module
    was involved.**

  This retires the "no module-qualified paths" gap and the API warts it drove.
  A follow-up pass (same session) did the std cleanup the feature unlocks:
  - `std::text` dropped its `len as byte_len` / `len as count` aliases — it
    imports `std::string` and `std::array` as qualifiers and writes `string::len`
    / `array::len` directly, the flagship case, since no `use` could bind two
    `len`s under one name.
  - **`StringBuf` moved to its own module `std::strbuf`**, which is what actually
    cleans the *intra*-module collisions a qualifier could not: `buf_len` /
    `buf_is_empty` / `push_str` (dodging `std::string`'s own `len` and
    `std::array::push`) became plain `strbuf::len` / `is_empty` / `push`. So
    `strbuf::len(b)` now sits beside `string::len(s)` and `array::len(xs)`, three
    modules spending the one good name. `std::string` keeps the String-shaped
    conveniences (`join`, `concat`, `repeat`, `from_chars`) and imports
    `std::strbuf` to build them — it reaches one module further now, but
    `std::strbuf` ships no impls, so nothing that depended on `std::string`
    shipping none is affected.

  Deliberately not done: glob imports; a module used as a value or bare type
  (`var x = string;` is "undefined variable", which is honest — a module is not a
  value); and re-exporting a module qualifier through `pub use` (a qualifier is
  not an item, so `mod_find_use_alias` skips module imports).

- **Padding to a width (milestone 34)** — `std::string` ships `pad_start`,
  `pad_end` and `pad_center`, so a rendered value can be laid out in a field.
  Design: `language.md` "`std::fmt`" / "`std::string`".

  This closes the last piece of "growing std on the natives" that had a design
  question behind it, and the milestone *is* the answer to that question: **the
  format spec is not a grammar.** `{}` stays a bare expression, and a rendering
  choice is a function beside the value — `std::fmt::float(f, 3)` for precision,
  these three for width — never a spec inside the braces. The reason is forced
  rather than chosen: **padding needs the value rendered to a `String` first**
  (you cannot align what you have not yet turned to text), so `pad` is
  `String → String`, and a `{v:>8}` grammar could only ever be sugar that
  desugars to `pad_start(v.to_string(), 8, ' ')`. The functions are the
  primitive; the grammar, if it is ever wanted, is a front-end convenience that
  changes no runtime — the same shape milestone 18 gave `"{v}"` itself.

  Three things follow, and they are the whole milestone:
  - **It shipped no compiler change at all** — no scanner, parser, AST, codegen
    or VM, and not even a native. All three functions are ordinary ducktape in
    `std/string.dt`, built on `strbuf` + `push_char` the same way `repeat` is.
    The whole feature is one `.dt` addition, which is milestone 14's claim
    ("std is written in ducktape") paying off at its cleanest.
  - **Width is a *character* count, not a byte count** — the display side of
    milestone 32's byte-vs-character fork. Alignment is a display question and a
    display question is about characters, so `pad_start("café", 6, ' ')` adds two
    spaces, not one. It is still not a *true* display width — a double-width or
    combining character counts as one — the same ASCII-exact limit `std::char`
    documents, and it inherits `chars`'s runtime error on invalid UTF-8.
  - **It lives in `std::string`, not `std::fmt`** — `pad` reshapes a `String`,
    where `float` renders a `Float`, so it is `repeat`'s neighbour rather than
    `float`'s. That also keeps the no-impl property: `std::string` ships no
    impls and refuses to reach `std::array` (the count is a `for` over `chars`,
    not `array::len(chars(s))`, for the same reason `join` counts nothing —
    reaching `std::array` would close the `string → option → cmp → string`
    cycle), so a program can pad without inheriting anything. In `std::fmt`,
    importing `pad` would have dragged in every `Display` impl that module ships.

  Deliberately not done: the `{v:>8}` sugar (recorded as the one thing genuinely
  absent, since the functions nest a call in the braces where a spec would read
  terse); truncation (pad only widens — narrowing is `slice`'s job and cuts in
  bytes); and an `Align` enum with a single `pad(s, w, f, align)` — three
  functions read better at the call site, and the enum only pays off if the sugar
  is ever built, at which point the wrappers are trivial.

- **The format spec `{v:>8}` (milestone 35)** — the sugar milestone 34 left as
  the one genuinely-absent piece. `{v:>8}` / `{v:<8}` / `{v:^8}` pad a rendered
  value to a width, `{v:'-'^8}` sets the fill, `{f:.3}` fixes the decimals, and
  `{f:>10.3}` fuses the two. Design: `language.md` "`std::fmt`", `architecture.md`
  "Interpolation and `Display`", `grammar.ebnf`.

  The observation the milestone turns on: **the spec is not a runtime, it is a
  spelling.** Milestone 34 established that padding needs the value rendered to a
  `String` first, so a spec could only ever desugar to the `pad_*` / `float`
  calls that already exist. So that is literally what `check_interpol_seg` does
  — render the value (through `std::fmt::float` for a precision, through a nested
  `{v}` interpolation otherwise, so a primitive and a `Display` type keep the
  render they always had), then wrap it in the matching `std::string::pad_*`.
  **Codegen, `OP_INTERP`, the VM and the image format did not change**, exactly
  as milestone 18 predicted a spec would behave: the segment simply evaluates to
  a `String`. The whole feature is a scanner token (`^`), a `FormatSpec` on the
  `InterpolSeg`, a parser for the spec, and the checker rewrite.

  Two things follow, and the second is the real cost:
  - **The scanner already tokenised the spec** — `:` `>` `8` are ordinary
    tokens, so the only new one is `^` for centre. The one quirk is that `8.3`
    lexes as a single FLOAT (a digit precedes the dot) while a bare `.3` lexes as
    DOT then INT, so a fused width-and-precision arrives as a FLOAT the parser
    splits, and a lone precision as its own tokens. The fill is a *char literal*
    (`'-'`), which tokenises unambiguously where a bare `*` would collide with
    multiplication and is the `Char` the value is padded with either way.
  - **`pad_*` and `float` had to become lang items, and that is forced by the
    same argument that made `Display` one.** The user never writes those names,
    so the compiler generates the calls — and if it resolved them through
    whatever happens to be imported, the meaning of `{v:>8}` would depend on
    imports, exactly the reason `Display` is captured rather than looked up.
    `tc_register_fun` captures `fmt_pad_start`/`fmt_pad_end`/`fmt_pad_center`
    from `std::string` and `fmt_float` from `std::fmt`, keyed on the module, and
    a spec whose module is absent is reported. This is the **second crack** in
    "the compiler knows nothing about std": one captured name became five, and
    the honest framing is that formatting as a whole — how a value renders *and*
    how it is laid out — is the corner of std the language is entangled with.

  The calls are built by `mk_fun_call`, which sets `resolved_fun` on a synthetic
  two-segment path directly rather than routing through `resolve_callee` (which
  resolves by name and would fail on a name the user never imported) — the same
  "the checker already knows the FunDef" shape `resolve_assoc_call` uses. The
  `pad_*` are non-generic ducktape and `float` a non-generic native, so the
  calls need no inference; the rewrite validates only the value's own type, since
  a precision applies to a `Float` and nothing else.

  Deliberately not done: a *dynamic* width or precision (`{v:>{n}}`) — the two
  are literals, and a runtime value there has no spelling; truncation (pad only
  widens, inherited from milestone 34); and a bare width with no alignment
  (`{v:8}`), which would need a type-dependent default the checker has no reason
  to pick, so an alignment is always required before a width.

- **`Ord` for the containers (milestone 36)** — `[T]` and `(A, B)` order
  themselves, so an array of strings sorts and a tuple compares field by field
  without a line of it written per program. Design: `language.md` "`std::cmp`".

  Like milestone 20's `Display` for the same two containers, **the std code
  needed no compiler change — both impls ran against the unmodified binary**,
  and the milestone is the placement question, not the code. Two things settle
  it, and the second is the one worth recording:
  - **Both impls live in `std::cmp`, beside the trait**, exactly where `std::fmt`
    keeps `Display for [T]` / `Display for (A, B)`: an array and a tuple have no
    module of their own, so the trait's module is the closest thing they have to
    one. Milestone 20 supplied the permission (module-granular impls, so shipping
    one no longer claims `[T]`'s ordering for every program) and milestone 20's
    bound-at-selection check makes `T: Ord` an error where the element has none.
  - **The array impl needs `len`, and `use std::array` would close a cycle** —
    `std::array` reaches `std::option`, which reaches `std::cmp`, so importing it
    back is a back edge the dependency graph rejects. The resolution is the
    milestone's one idea: **an impl is owned by the module that writes it, but an
    `@intrinsic` is not a definition at all — it is a spelling for an opcode.**
    `OP_LEN` is program-wide; `std::cmp` declares its own private `len` and names
    the opcode a second time, which costs nothing and depends on nothing, so the
    cycle that blocks the impl's *type* does not block the *length* it needs. The
    tuple impl needs none of this — `.0`/`.1` are field reads — and ships free.

  Arity stays per-impl (`(A, B)` only, nothing is generic over a tuple's length,
  the same limit `Display` records), and the element is ordered through its own
  `Ord`, so `[(A, B)]` and `[[T]]` sort by reaching both new impls through the
  bound (`tests/run/container_ord.dt`). Not done: `Ord` for other tuple arities,
  and a total order for `[Float]` still inherits the NaN wart the element does.

- **`==` on a generic, and the soundness hole behind it (milestone 37)** — a
  binary operator on two values of an unbounded generic type used to be a bare
  `// todo:` in the binary resolver that returned `t_poison` unconditionally and
  silently. Removing it is the whole fix; the per-operator rules below it already
  say the right thing. Design: `language.md` operators.

  The observation the milestone turns on: **structural equality is a runtime
  operation, so `==` on a generic needs no static type.** `OP_EQ` inspects none —
  it compares two structs the same way it compares two Ints — so `a == b` on a
  generic `T` is `types_equal(T, T)` → `Bool`, exactly as it already was for a
  struct or a tuple. Arithmetic and ordering are the other side of the same coin:
  there is no operator overloading, so `+` and `<` want a concrete numeric type,
  and a generic operand now reports against its own name ("requires numeric
  types, got 'T'") instead of poisoning. A bounded `T: Ord` still orders through
  `.cmp`, which is why `std::cmp` and `std::option` never wrote `a == b` on a
  generic and the hole stayed hidden.

  The reason it was worth a milestone and not a one-line cleanup is the *second*
  bug the silent poison caused. A poisoned `if` condition makes `resolve_expr`'s
  `EXPR_IF` bail before the branch body (`type_is_poison(cond_ty)` → return), so
  the body went **unchecked** — a blatant `var z: Int = "str";` inside
  `if a == b { .. }` was accepted, and a *call* in that body reached codegen with
  no `resolved_fun`, failing as the misleading "this name is not supported by the
  VM yet". So one silent poison in the type layer surfaced as a soundness hole in
  one direction and a codegen crash-diagnostic in the other, and neither pointed
  at the operator. The lesson is the one the CLAUDE.md convention already states:
  a checker error is *one `diag_error` then poison* — a poison with no diagnostic
  is how a whole branch goes dark. Tests: `tests/run/generic_eq.dt` (the positive
  path across Int/String/struct, and the call-in-`if`-body shape the bug hid),
  `tests/fail/generic_add.dt` / `generic_cmp.dt` (arithmetic/ordering now
  diagnosed), `tests/fail/generic_eq_body_checked.dt` (the body error now
  reported). Uncovered while prototyping a `std::assert`, which `assert_eq<T>`
  needs and which is the natural next piece.

- **Ordering operators over `Ord` (milestone 38)** — `a < b` on a non-numeric
  type desugars to `std::cmp::Ord`, the way `"{v}"` desugars to `Display`.
  Design: `architecture.md` "Ordering operators and `Ord`", `language.md`
  operators / `std::cmp`.

  The observation the milestone turns on is the same as interpolation's, one
  operator over: **once a trait decides ordering, `a < b` is not an operator on
  `a` and `b` — it is the numeric comparison `a.cmp(b) < 0`.** So
  `rewrite_ord_comparison` reshapes the `EXPR_BINARY` node in place: build an
  `EXPR_METHOD_CALL` for `a.cmp(b)`, resolve it against the already-known
  receiver type, then set the node's left to that call and its right to a `0`
  literal, operator untouched. The outer node is now `Int OP Int`, so **codegen,
  `OP_LT` and the VM did not change at all** — the exact shape the `to_string`
  rewrite has, where the segment ends up a `String` the VM already stringifies.

  Three things follow, and they are the whole milestone:
  - **Numeric comparison stays a built-in opcode**, and so does *all* of
    `==`/`!=`. This is the split `Display` drew: `1 < 2` keeps `OP_LT` with no
    import and no frame, exactly as `"{1}"` keeps the VM's own rendering; only a
    *non-primitive* operand reaches for the trait. `Ord` becomes a lang item as a
    result (`TypeChecker.ord_trait`, captured in `tc_register_trait`, keyed on
    `std::cmp` like `display_trait`), so `<` on a user type is import-dependent —
    but that regresses nothing, since `<` on a struct was an *error* before.
  - **Only `cmp` is named.** Rewriting to `cmp` compared to `0`, rather than to
    the `lt`/`le`/`gt`/`ge` defaults, keeps the lang-item surface a single
    method — the smaller entanglement, matching how `Display` names exactly
    `to_string`. The four defaults stay ordinary trait items, callable directly
    (`a.lt(b)`), just not what the operator reaches for.
  - **`ord_satisfied` gates the rewrite** before it happens, for the two reasons
    `display_satisfied` does: the diagnostic stays about *comparison* — naming
    the `T: Ord` bound to add for a type parameter, or the missing impl (with the
    unimported-impl / blocking-bound notes) for a concrete type, or the absent
    `std::cmp` import — rather than degrading to "no method named 'cmp'", and an
    unrelated inherent `cmp` cannot silently qualify.

  `==`/`!=` are deliberately *not* included: structural equality is a free,
  import-less, universal primitive (`OP_EQ` reads no static type), and routing it
  through an `Eq` would make the commonest operation import-dependent and hand
  coherence the power to take `[1,2] == [1,2]` away from a program. `Eq` waits
  for a concrete consumer that needs *custom* equality. Tests:
  `tests/run/ord_operators.dt` (struct, bounded generic, Char, `max`),
  `tests/run/string_ord.dt`'s selection sort now spells its comparison `<`,
  `tests/fail/generic_cmp.dt` (unbounded `T` asks for the bound),
  `tests/fail/ord_not_implemented.dt` (a type with no impl),
  `tests/fail/char_no_comparison_operator.dt` (the absent-import case).

- **Native methods (milestone 39)** — a method's body may be `@native`/
  `@intrinsic`, so a primitive's operation can be spelled `s.len()` rather than a
  free `string::len(s)`. Design: `language.md` "Native functions", `runtime.md`
  "Native functions", `grammar.ebnf` `implItem`.

  The observation the milestone turns on: **`self` is an ordinary parameter to
  the C function** — the same value the free-function form passes as argument 0.
  So a native method is not a new mechanism, it is the existing one with the
  receiver named `self`, and almost nothing below the signature had to move:
  - **The runtime did not change at all.** `compile_method_call` already pushed
    the arguments in `is_self` order and closed with `OP_CALL`, and `OP_CALL`
    already dispatched `fun->native` — so an `@native` method runs, takes a
    global slot, and drops into a `dyn Trait` vtable with no new code. The one
    codegen addition is the `@intrinsic` method, which mirrors `compile_call`:
    push the receiver at its `self` position and emit the opcode inline, so the
    "an intrinsic has no slot" diagnostic is never reached for a method.
  - **The checker change is two lines of reuse.** `resolve_impl_decl` runs the
    same `tc_bind_native` on a method's `FunDef` that `tc_register_fun` runs on a
    top-level one — so an unknown name reports against its span, exactly as
    before — and `tc_check_impl` skips a bodyless method's body-check the way
    `tc_check_fun` already skips a native's. The parser accepts an optional
    attribute before `fun` in an impl body and threads it through the same
    `parse_fun_decl`.
  - **A *trait*-declaration method stays non-native.** Its default body is
    generic over `Self`, so there is no concrete C body to bind; the attribute is
    accepted on a top-level `fun` and on an impl method only.

  **The standard library deliberately does not adopt it here.** Every std module
  documents its free-function spelling as a choice — `std/strbuf.dt` and
  `std/char.dt` both note "ships no impls, and is free to reach for", the
  property that keeps them cheap dependencies — and several call sites are
  load-bearing for unrelated tests (`tests/run/mod_qualified` leans on the free
  `array::len` to demonstrate module-qualified paths). Migrating std to methods
  would have to rethink each of those notes, so it is left as a separate step;
  this milestone lands the *capability* the way milestone 16 landed natives, with
  the registry deliberately small. Tests: `tests/run/native_method.dt` (a native
  `String` method, an intrinsic `[T]` method, and a native method dispatched
  through a `dyn Measure` vtable), `tests/fail/native_method_unknown.dt` (an
  unknown native name on a method reports at its span).

- **The standard library adopts methods (milestone 40)** — the primitive modules
  now spell their operations as methods: `s.len()`, `xs.push(v)`, `c.code()`,
  `b.build()`, with constructors as associated functions (`StringBuf::new()`,
  `Char::from_code(n)`). Design: `language.md` "The standard library",
  `runtime.md` "Native functions". This is the milestone-39 follow-up, and it is
  **a pure `.dt` change**: no C moved, the checker and runtime were already done
  in 39, and the whole diff is the four primitive modules (`std::string`,
  `std::array`, `std::strbuf`, `std::char`), their dependents' call sites, and
  the tests. `make sanitize` stayed clean because there was nothing new to make
  unsafe.

  The observation the milestone turns on: **a method needs its defining impl
  visible, so where a free function only needed a name, a method needs a
  reachable module.** That is what draws the line between what migrated and what
  could not:
  - **The forced exception is `std::cmp`'s array length.** `impl Ord for [T]`
    needs an array's length, but `std::cmp` cannot `use std::array` — that closes
    the `array → option → cmp` cycle — so it cannot reach `xs.len()` *as a
    method* either, because the method would need array's impl in view. It keeps
    the private free `@intrinsic fun len<T>(xs)` it already had: an intrinsic is a
    spelling for an opcode, not a definition owned by a module, so it dodges the
    cycle a method cannot. The same free spelling that milestone 39's note called
    load-bearing turns out to be load-bearing for exactly this reason.
  - **`compare` and `code` migrated freely**, because `std::cmp` already imports
    `std::string` and `std::char` (no new cycle), so `self.compare(other)` and
    `c.code()` resolve against impls already in view. The imports changed from
    naming an item (`use std::string::compare`) to naming the module (`use
    std::string`) — a method travels with its impl by reachability, not through a
    named import, so the `use` is only there for the dependency edge now.
  - **The lang items and the builders stay free.** `pad_start`/`pad_end`/
    `pad_center` and `float` are what the `{v:>8}` / `{f:.3}` format spec desugars
    into, so their meaning cannot depend on what a program imported — a method
    would move the target the desugar builds and break the capture in
    `tc_register_fun`. `join`/`concat`/`from_chars` stay free because their
    receiver is a `[String]` / `[Char]`, not a `String`. `print` stays free
    because it is general over any `T`, a function rather than one type's method.

  **The cost the milestone actually buys is a wart, and it is worth naming.**
  Shipping `impl String` / `impl<T> [T]` / `impl Char` widely means a program that
  imports the module can no longer add its own inherent method of the same name to
  that primitive — overlapping inherent methods are silently first-wins (there is
  no coherence check on inherent impls), so it shadows with no diagnostic. This is
  the `Display`/`Ord`-for-containers tradeoff, one level over, minus even the
  error: a trait impl coherence would reject, an inherent one it accepts.
  `std::strbuf` is the free case — `StringBuf` is a type only it defines, so no
  program competes for those names.

  One consequence surfaced in the tests and is the honest edge of the change: **a
  method is not a first-class value.** A free `@native` could be stored, passed,
  and captured (`var f = str_len;`); a method cannot be named as a path-value at
  all. So `tests/run/native.dt` (which exercises exactly that first-classness) and
  the `intrinsic_as_value` / `unreachable_body` tests now declare their own *free*
  natives to name — which is legitimate, since `@native`/`@intrinsic` were never
  restricted to `std`. The property belongs to free functions; the method spelling
  trades it for the receiver syntax. New test: `tests/run/std_methods.dt`
  exercises all four modules' method surface in one file.

- **`std::text` adopts methods (milestone 41)** — the searching/splitting/
  parsing layer now spells its operations as methods on `String`:
  `s.starts_with(p)`, `s.ends_with(p)`, `s.find(n)`, `s.contains(n)`,
  `s.split(sep)`, `s.trim()`, `s.parse_int()`. Design: `language.md` "The
  standard library" → `std::text`. Like milestone 40 it is **a pure `.dt`
  change** — the whole diff is `std/text.dt`, its one test, and this note; no C,
  and `make sanitize` had nothing new to make unsafe.

  The milestone is milestone 40's rule read from the far side: **a method needs
  its defining impl reachable, and `std::text` is exactly the module the cycle
  put out of reach of `std::string`.** Milestone 40 migrated the primitives'
  *own* operations but left `std::text` as free functions, because it is not
  `std::string` — the `string → option → cmp → string` cycle forces the
  `Option`- and `[String]`-returning operations one module up. That did not
  block the migration: an `impl String` is legal in any module that can see
  `String`, so `std::text` ships a **second `impl String`** beside the leaf's.
  Two inherent impls for one type coexist with no coherence question because
  their method names are disjoint and neither is a trait impl — `impl_index_*`
  finds each name in whichever *visible* impl declares it, so a call site sees
  `s.len()` (the leaf's) and `s.trim()` (this one) as one flat method surface
  and never learns they live in different files. The module boundary the cycle
  draws stays invisible exactly where it should: at the call site.

  It extends the milestone-40 wart by the same mechanism — an inherent method on
  a primitive shipped widely means a program importing `std::text` cannot add its
  own `String` method of these names — and buys nothing new in the backend, which
  is the point: the whole feature is turning seven free functions into a `self`
  parameter and updating the one test that named them. The internal cross-call
  `contains` → `find` became `self.find(needle)`, an ordinary method call on the
  receiver.

- **`Ord for Float` is a total order (milestone 42)** — the last of the ordering
  warts: IEEE comparison is not a total order, so the naive three-branch `cmp`
  returned 0 for `NaN.cmp(x)` at every `x` and NaN compared *equal to
  everything*, making `max(nan, 1.0)` and `max(1.0, nan)` disagree and a
  `[Float]` with a NaN in it have no defined sort. Design: `language.md` →
  `std::cmp`, and the wart entry updated to record the decision.

  The observation the milestone turns on: **the fix needs nothing the language
  cannot already say.** `self != self` is the NaN test — only NaN is unequal to
  itself, and `!=` is IEEE on `Float` because `value_equal` compares the bits
  with `==` — so the whole change is two branches at the top of `impl Ord for
  Float`, a pure `.dt` edit with no compiler, opcode or runtime change. The only
  real content is the *decision* the wart said "nothing has needed to make":
  **NaN sorts after every real number and all NaNs are equal to each other.**
  That is the placement that keeps `cmp` transitive (every real `x` is `< NaN`,
  and NaN is neither `<` nor `>` another NaN) while ignoring the sign bit and
  payload, so a `-NaN` and a `+NaN` are one order — deliberately coarser than
  Rust's `total_cmp`, since nothing distinguishes them. `max`/`min` now give the
  same answer whichever side the NaN is on (`tests/run/float_nan.dt`), which is
  the concrete thing the total order buys.

- **Mutable aggregate fields (milestone 43)** — `p.x = v`, `self.n += 1`,
  `t.0 *= 2` and `xs[i].field = v` all run. Design: `runtime.md` "Codegen
  shapes" + the `OP_FIELD_SET` row, `language.md` assignment.

  The observation the milestone turns on: **field assignment already
  type-checks.** `EXPR_ASSIGN` resolves its target as an ordinary expression and
  unifies it with the RHS, so `p.x = v` passed sema the whole time — it died
  only at codegen with "assignment to this target is not supported by the VM
  yet." So the milestone is a pure backend feature, the mirror of a getter that
  already existed: `OP_FIELD_SET index` pops `[obj, value]`, writes
  `fields[index]`, and pushes the value back (assignment is an expression),
  exactly as `OP_FIELD_GET` reads it. It works over every aggregate the getter
  reads — struct, tuple element, enum-variant field — because it dispatches on
  the same three `Obj` kinds.

  Three things fall out, and they are the whole milestone:
  - **`compile_assign` grows one branch.** A field target routes to
    `compile_field_assign`, which for a plain `=` compiles receiver then value
    then `OP_FIELD_SET`, and for a compound `+=` uses the existing `OP_DUP 0` to
    read the current field off a duplicated instance before combining — so the
    receiver expression is evaluated once, the same discipline
    `compile_index_assign` already keeps for `arr[i]`.
  - **Nested and array-element targets are free.** `w.c.n = v` and
    `xs[i].n = v` need no case of their own: the receiver is whatever expression
    the target's `.object` is, compiled recursively.
  - **Nothing else moved.** No `mut` keyword (locals have none — all vars are
    mutable), no aliasing analysis (a struct is a shared heap reference, so a
    write through `self` or an alias is visible everywhere, the rule arrays
    already keep), and the serializer is untouched because it copies raw chunk
    bytes and never decodes an opcode.

  This is the foundational gap behind a stateful object: until now a struct
  field could be read but never written, so a counter could not count and an
  iterator could not advance a cursor — the immediate blocker met while scouting
  an `Iterator` trait as the next feature. `tests/run/field_assign.dt` covers
  plain/compound assignment, mutation through a method, a nested field, a tuple
  element, aliasing, and a field of an array element.

- **Field-level visibility (milestone 44)** — a struct field carries its own
  `pub`, private by default. A `pub struct` stays importable, but each field is
  readable, assignable, constructible, and matchable from another module only
  when written `pub`; within the defining module every field is reachable
  regardless. Design: `language.md` visibility section, `architecture.md`
  "Field visibility is the one check that is not at the import boundary".

  The observation the milestone turns on: **every field use already funnels
  through one function.** `find_struct_field` answers construction
  (`EXPR_STRUCT_INIT`), `.field` access-and-assignment (`EXPR_FIELD` — a write
  reaches it through its assignment target, so milestone 43's `OP_FIELD_SET` is
  covered for free), and a struct pattern. So visibility is one guard,
  `check_field_visible`, called at those three sites: `field->is_pub ||
  accessing_module == def->module`, the accessing module read off
  `CheckCtx.tyres.module`. Item visibility is checked at the import boundary
  (`link_import_item`) because an item is imported by name; a field never is —
  it is reached through a value — so its check has to live at each use instead.

  Three things fall out, and they are the whole milestone:
  - **Parser gains one optional token.** `parse_field_decl` accepts a `pub`
    before a named or tuple field. An enum variant's payload reuses the same
    function but passes `allow_pub = false` — a variant is as visible as its
    enum, so a `pub` there is rejected rather than silently ignored.
  - **Codegen and the image are untouched.** Field *indices* do not change, so
    the check is pure front-end — the same "serializer never moved" property
    milestone 43 had.
  - **The default flip cost one line each in five tests.** No `std` struct
    exposes a field cross-module, and only five pre-existing multi-file tests
    read an imported struct's fields; each gained `pub` on the fields it always
    meant to expose. `tests/run/field_visibility` is the honest private-cursor
    `Counter`; `tests/fail/field_private{,_pattern}` and
    `tests/fail/field_pub_on_variant` cover the three rejections.

  This is the encapsulation milestone the `Iterator` scouting wanted next: an
  iterator's whole point is a private advancing cursor, which until now a
  `pub` type could not hide. Enum variants keep no field-level visibility (a
  `pub enum`'s payloads are as visible as the enum), matching Rust.

- **A prelude (milestone 45)** — the `Iterator` scouting turned up a friction
  worth fixing at the root first. ducktape has no auto-import, and a lang item
  is a name the *compiler* resolves for a construct that never spells it — so
  interpolating a struct silently needed `use std::fmt`, `a < b` on a struct
  needed `use std::cmp`, a `{v:>8}` spec needed `use std::string`. Each is a
  syntactic construct whose meaning lived behind an import the user had to
  remember. The prelude removes the whole class: every non-std module implicitly
  imports `std::option` (`Option`), `std::result` (`Result`), `std::cmp`
  (`Ord`), `std::fmt` (`Display`), and `std::string` (capture-only, for the
  `pad_*` a width spec needs). Design: `architecture.md` "Modules" → "The
  prelude is discovery, not a new phase".

  The observation the milestone turns on: **the prelude is discovery, not a new
  phase.** Every later phase — dep graph, register, resolve, link — already
  iterates one array, `Module.imports[]`. So the prelude is *synthesised import
  entries* appended to that array (`mod_inject_prelude`, in `src/module.c`,
  right after `mod_collect_imports`): ordinary `DECL_USE` nodes carrying a new
  `from_prelude` flag. Nothing downstream changed. Three properties are the
  whole design:
  - **std is exempt** (`is_std_key`): a prelude module can't import itself, the
    one cycle to avoid.
  - **Lowest priority, silent yield.** Appended *after* the real imports (so
    those link first), and on a name already bound — an own decl or an explicit
    import — `link_import_item` sees `from_prelude` and returns quietly
    (`link_name_bound`) instead of the clash diagnostic. This is Rust's "a local
    item shadows the prelude": `tests/pass/prelude_shadow.dt` keeps its own
    three-variant `Option`, and `tests/*/propagate` keep their hand-written
    `Result` + `?`.
  - **Lang-item capture is free.** `display_trait`/`ord_trait`/`fmt_*` (see
    `sema.h`) are captured when their module *registers*, which the prelude now
    guarantees always happens — so they went from "NULL unless imported" to
    always-populated.

  Two things fall out:
  - **Four "you forgot to import std::X" diagnostics became unreachable** — the
    invisible import can no longer be missing. `tests/fail/{interp_no_fmt,
    char_no_comparison_operator,fmt_spec_no_fmt,fmt_spec_no_string}` were retired
    (the first was already a duplicate of `interp_no_display`); their coverage
    moved to `tests/run/prelude.dt`, which reaches every lang item with only
    `use std::io::print`. The NULL guards stay as defence against a std module
    failing to load.
  - **`print` is deliberately *not* preluded.** It is a plain function tied to
    no syntax, so it stays `use std::io::print` — preluding it would have churned
    nearly every test to fix no friction. The prelude is exactly the lang-item
    and vocabulary modules, nothing more.

  With the friction gone at the root, the deferred `Iterator` trait is now the
  clean choice for next: it can name `Option` and `Display` in its signatures
  and be *used* through a bare `for x in it` without the loop ever forcing an
  import.

- **`@lang` items + `for` over an `Iterator` (milestone 46)** — landed in two
  commits, a mechanism then its first consumer.

  **`@lang`, a marker attribute.** A lang item is a std definition the compiler
  resolves for a construct that never spells it (`"{v}"` → `Display`, `a < b` →
  `Ord`, a `{v:>8}` spec → `pad_*`/`float`). These were six hard-coded
  `mod_is_std(m,"fmt") && name=="Display"` matches in the checker — the fact
  "this trait is `Display`" living in C, keyed on the spelling. The std source
  now declares it locally with `@lang("display")`. Unlike `@native`/`@intrinsic`
  it is a *marker*, not a body, so it sits on a bodied trait/enum/fun and
  composes with a body attribute (`float` carries both); `parse_decl` sorts a
  decl's attributes into a body attribute (`DeclFun.attr`) and this marker
  (`Decl.lang_attr`). Honoured only inside the embedded standard library and
  **inert** elsewhere — a user's `@lang` can't claim a lang item (so can't
  hijack what `"{v}"` means), but isn't rejected either, because a std file run
  by path carries the same marker yet isn't the `<std>/` key, and "a std file is
  an ordinary module, directly runnable" is worth keeping (`mod_is_std_module`
  gates it). A pure refactor: the Display/Ord/spec tests proved equivalence.

  **`Iterator`, its first fresh consumer.** `for x in it` over a value that is
  neither an array nor a range now requires `it` to implement `Iterator`
  (`std::iter`, `@lang("iterator")`, preluded): `trait Iterator { type Item; fun
  next(self) -> Option<Self.Item>; }`. The loop drives `it.next()` — `Some(x)`
  binds and runs the body, `None` ends it — and the cursor advances *in place*,
  which is exactly the private mutable field milestones 43 and 44 were scouted
  for. Two moves, each mirroring a feature already here:
  - **Nominal gate, structural unwrap.** The type must implement the trait
    (`impl_index_implements`, the question `display_satisfied` asks — so the
    diagnostic names the missing impl, and a bare `next()` on an unrelated type
    is *not* enough). But the `Option` `next` returns is taken apart by shape
    (`enum_is_optionish`, the `Some`/`None` sibling of `?`'s `enum_is_resultish`),
    so nothing is tied to std's `Option` beyond its two variants. This is the
    same split `==`/`?` take: the contract is nominal, the value is structural.
  - **No new codegen shapes.** `compile_for_iter` is `compile_for_array`'s
    scaffold (the iterator in a hidden local, so a re-evaluated `it` can't
    restart it) plus `compile_propagate`'s unwrap (tag test, then field 0).
    `continue` is backward like a `while`'s. The checker synthesises `it.next()`
    the way interpolation synthesises `v.to_string()` and leaves the resolved
    call + variants on the `ExprFor`.

  `@lang` paid off immediately: `std/iter.dt` self-declares `@lang("iterator")`
  rather than adding a seventh magic match. Design: `architecture.md` "Lang
  items" / "`for` over an iterator", `runtime.md` `compile_for_iter`,
  `language.md` "Iterators". `tests/run/iter.dt` is the private-cursor `Counter`
  (plus a `String`-yielding iterator and break/continue); `in_fixed.dt` gains
  one; `tests/fail/for_not_iterator.dt` is the non-`Iterator` rejection.

  Deliberately left for later: `Iterator` ships only `next` — no `map`/`filter`/
  `collect` combinators yet (they want closures over an iterator and a growable
  sink), and a call *through* a `T: Iterator` bound still can't run, since
  codegen rejects generic functions. `for` over a concrete iterator is the
  runnable slice, and it is the one that needed the private cursor.

- **Iterators through a bound + the first combinators (milestone 47)** — the two
  things milestone 46 deferred, and the small inference fix that stood between
  them. `std::iter` ships `map`, `filter`, `collect`.

  **`for` over a bounded/`dyn` iterator.** Milestone 46 drove only a *concrete*
  iterator; a `for x in it` where `it` is a generic `I: Iterator` or a
  `dyn Iterator` hit codegen's "this iterator is not supported by the VM yet".
  The cause was exactly the one a bounded *method* call already solved: the
  checker resolves `it.next()` against the trait *signature*, so the node carries
  `bound_trait`/`bound_self` and no `resolved_method`, and `compile_for_iter`
  only read the concrete fields. The fix is the three-way target selection
  `compile_method_call` has had since milestone 10 — concrete method, inherited
  default, or **dispatch through a bound** (substitute the receiver to a concrete
  type and re-run `cg_bound_target`, monomorphising the body; or, for a `dyn`,
  pick the slot off the vtable with `OP_DYN_METHOD`). No new runtime shape: the
  loop scaffold is unchanged, only *which* `next` it emits. The checker's nominal
  gate also learned to admit a `dyn Iterator` directly (it *is* the trait rather
  than implementing it through the index).

  **The combinators are ordinary library code, and that is the whole point.** An
  adapter is a struct with a `fun(..)` field and an `impl Iterator`; `map`/
  `filter` are lazy (each wraps its source and pulls one element per `next()`),
  `collect` drains into a native-backed array. Nothing here is built into the
  language — a program can write the same shapes — which is milestone 14's claim
  read once more: a std module needs no compiler support the language doesn't
  already give a user.

  What *did* need a compiler change was one inference gap, and it is the
  interesting part. `map`'s closure parameter is typed `I.Item`, an associated
  projection; the closure body (`|x| => x * 2`) cannot be checked while `x` is an
  abstract `I.Item` — a projection is neither numeric nor comparable. The element
  type *is* known — the first argument (the iterator) pins `I` — but
  `resolve_call_expr` hinted each argument with the raw `param_types[i]`, so the
  second argument's hint still read `I.Item` after the first had solved `I`. The
  fix is one `infer_apply` on the parameter type before it becomes the hint:
  **arguments are checked left to right, each hinted by what the earlier ones
  solved.** Once `I = Counter`, `I.Item` collapses to `Int` and the closure
  checks. The mirror fix — `infer_apply` on the `for`-loop's element type — is
  what lets a `Filter` (whose `type Item = I.Item`) bind a usable loop variable
  rather than an abstract `Counter.Item`.

  Two design lines worth recording:
  - **`collect` drains with `while it.next()`, not `for x in it`.** The `for`
    desugaring's gate is the `iterator` lang item, which is *inert* when a std
    file is run by path (it is not the `<std>/` key), so a `for` inside
    `std::iter` would break "every std file is directly runnable" — a property
    milestone 46 went out of its way to keep. A bound method call needs no lang
    item, so the `while` form is the one that survives both ways of loading the
    file.
  - **The projection-through-a-bound *codegen* wart still stands.** `map`/
    `filter` take their source apart with `match`, never by handing a projection-
    typed value to another generic. `it.next().unwrap()` — a projection into the
    generic `unwrap<T>` — still fails at codegen ("type argument 'T' is not known
    here"), because codegen has no `infer_apply` to collapse `I.Item` once the
    base is concrete. The combinators are written to avoid it; the wart is
    narrowed, not closed.

  Design: `language.md` "Iterators" → combinators, `runtime.md` `compile_for_iter`,
  `architecture.md` "`for` over an iterator" / "Types and inference".
  `tests/run/iter_combinators.dt` exercises map/filter/collect, a chained
  pipeline, `for` over a bounded generic and a `dyn Iterator`, and break/continue
  through a filter; `in_fixed.dt` gains a combinator line.

  Deliberately left for later: no `fold`/`enumerate`/`zip`/`take` yet (each is
  the same adapter shape, added when wanted), and no `flat_map` (a closure
  yielding an iterator to flatten needs the projection-through-a-bound codegen
  the wart above describes).

  **Combinators are free functions, not `Iterator` methods** — `collect(map(it,
  f))`, not `it.map(f).collect()`. Two things blocked the method form, and one is
  now fixed. (a) *Name resolution*: a method returning an adapter (`-> Map<Self,
  B>`) and the adapter naming the trait back (`struct Map<I: Iterator>`) is a
  definition cycle that source-ordered resolution could not close. This is fixed
  in a separate commit — `tc_resolve_module` is now two sub-passes, a declare
  pass binding every type name before a body pass fills signatures, so the cycle
  resolves and a trait method may return a later-defined adapter (`architecture.md`
  pass 2). (b) *Object safety*, still open: a combinator is generic or returns a
  `Self`-composed type, so as a trait method it can't sit in a vtable and
  `trait_check_object_safe` rejects the whole trait for `dyn` use — and this
  milestone added `dyn Iterator`. The production fix is Rust's `Self: Sized`
  read as a mechanism: compute object safety over the *dispatchable* method
  subset only, auto-excluding a provided method that can't be dispatched (not in
  the vtable, not object-safety-checked, an error to call through a `dyn`). With
  that, map/filter/collect move onto `Iterator` and `dyn Iterator` survives. Not
  done here; the free functions are the honest interim.

## Next (in recommended order)

Estimates are relative to one focused session ≈ the checker-completion
milestone (~900 lines).

Nothing on the main line is *blocked*: every construct the checker accepts in
`tests/pass/in_fixed.dt` now also runs. The list below is what the "known
warts" section would promote first, in the order that pays off soonest — pick
by appetite rather than by necessity.

1. **Growing std on top of the natives** — the mechanism landed in milestone
   16 with a deliberately small registry, and the pieces with a design question
   behind them are done: a growable `ObjArray` (milestone 23, so `std::array`
   has `push`/`pop`), a growable text buffer (milestone 24, so a `String` can be
   *built* rather than concatenated), and string ordering (milestone 25, so
   `impl Ord for String` exists and text sorts). What is left is breadth, and
   each piece is one registry entry plus a decision about the type it needs.
   Every piece with a design question behind it is now done — the last open one,
   padding, is milestone 34 (below).

(**The first iterator combinators** — `map`/`filter`/`collect` — are now
milestone 47, along with driving a bounded generic or a `dyn Iterator` through
`for`. They are ordinary `.dt` code (an adapter is a struct with an `impl
Iterator`); the one compiler change was inference, so a closure typed by the
source's `Item` projection can be checked. `fold`/`enumerate`/`zip`/`take` are
the same adapter shape when wanted; `flat_map` waits on the
projection-through-a-bound codegen wart.)

(**Padding a rendered value to a width** was the last open piece here and is now
milestone 34: `std::string` ships `pad_start`/`pad_end`/`pad_center`. The
format-spec question it was gating is answered *the functions are the primitive*
— a rendering choice is those functions, and the spec is sugar over them.)

(**The `{v:>8}` format spec** — the sugar milestone 34 recorded as the one thing
genuinely absent — is now milestone 35: a `:` spec desugars in the checker to
the `pad_*` / `float` calls, so codegen and the runtime are untouched. Its one
cost is that those four functions became lang items like `Display`, forced by
the same argument: the user never types the names, so their meaning cannot be
left to imports.)

(**Text operations** was the second open piece here and is now milestone 32:
`std::text` ships `find`, `split`, `trim`, `starts_with`/`ends_with`, `contains`
and `parse_int`. The byte-vs-character fork it left open was answered both ways
— a position is a byte offset, a classification crosses to `chars` — and the
functions had to become their own module rather than more of `std::string`,
which the dependency graph forced.)

(**Selection by argument type** was item 2 here and is now milestone 30: a
qualified `Meters::from(x)` reads its argument to pick the impl. The *receiver*
spelling that milestone 30's note left unread is now milestone 31: a
trait-qualified `Into::<Fahrenheit>::into(c)` names the trait so the receiver
settles the rest, without an expected type.)

(**Ordering operators over `Ord`** was item 2 here and is now milestone 38:
`a < b` on a non-numeric type desugars to `a.cmp(b) < 0`. The open question it
recorded — how many `Ord` methods the operator names — was answered `cmp` only,
the smaller entanglement, matching how `Display` names exactly `to_string`.)

2. **A custom equality trait (`Eq`), if a consumer ever needs it.** Not on the
   main line, and deliberately deferred rather than planned — recorded here so
   the reasoning survives. `==`/`!=` stay a structural primitive: `OP_EQ` reads
   no static type, so `a == b` works on any value — Int, struct, generic `T` —
   with nothing imported. Routing it through an `Eq`/`PartialEq` trait would
   (a) make the commonest operation depend on a trait impl — and unlike `Ord`,
   `==` is deliberately *not* one of the prelude's lang items, so this would be
   real friction, not the kind the prelude removes, (b) hand coherence the power
   to take `[1,2] == [1,2]` away from a program whose modules disagree — the
   `Display`/`Ord`-for-`[T]` wart applied to `==` — and (c) cost a monomorphised
   call where an opcode
   stands now, all to buy *custom* equality that would need specialisation (which
   the language does not have) to coexist with the structural default. Nothing
   needs custom equality yet, so `Eq` waits for a concrete consumer — a hash map
   keyed by user types, or a first type whose equality is genuinely not
   structural. The `PartialEq`/`PartialOrd` split waits with it; its one live
   motivation, the `Ord for Float` NaN wart, is an ordering bug fixable on its
   own terms and does not need the trait hierarchy to address.

Not on the roadmap: the **REPL** is a side feature, not a milestone — it lives
on the `feature/repl` branch (`--repl`, incremental compilation over one module
via `Module.decl_base`) and is not part of the main line.

## Known warts to clean up opportunistically

- a format spec inside `{}` exists as of milestone 35 (`{v:>8}`, `{f:.3}`,
  `{f:>10.3}`), desugaring to `std::string::pad_*` / `std::fmt::float`. What has
  no spelling is a *dynamic* width or precision (`{v:>{n}}`): both are literals,
  so a runtime value in that position must still be written as the call itself.
  A bare width with no alignment (`{v:8}`) is also rejected — an alignment is
  required before a width, since defaulting it would need the value's type
- a `Display` impl whose body is `return "{self}";` recurses until the frame
  limit. That is exactly how `std::fmt`'s four impls are written, and it is
  correct there only because a primitive receiver takes the built-in path —
  the asymmetry is invisible from the source. A cycle check would have to run
  where the impl is written, not where it is called
- `Display` ships for `[T]`, `(A, B)`, `Option<T>` and `Result<T, E>`, which
  means a program can no longer write its own for those: naming the trait makes
  the std impl visible and coherence rejects the pair. A tuple of any other
  arity has no impl, since nothing can be generic over a tuple's length
- `std::convert`'s blanket `impl<T, U: From<T>> Into<U> for T` is the same wart
  taken to its limit: it applies to *every* self type, so importing the module
  takes `Into` impls away from a program entirely. That is Rust's rule and the
  price of the free direction being free, but it is the widest thing coherence's
  blindness to bounds has cost so far
- a unit struct's name cannot be bound as a variable any more: `var Marker =
  7;` is a struct pattern against an `Int`, and the diagnostic ("expected
  struct type in struct pattern") describes the rewrite rather than the
  mistake. Rust behaves the same way; the message could be kinder
- no shadowing diagnostics for `var` (`vscope_define` todo); top-level item
  names do collide, but a `var` may silently shadow one in the same scope
- a refutable `var` binding whose column type inference never pinned down is
  accepted (the tri-state answer reports nothing) and traps at runtime via
  `OP_MATCH_FAIL` instead of at compile time
- `pub` is parsed and ignored on the `impl` keyword itself, but *rejected* on
  the items where visibility would have to be a thing: `pub fun` inside an
  `impl` block is "expected impl item", and `pub x` on a struct field is
  "expected field name". Method and field visibility do not exist, so std impls
  and structs simply omit it
- a definition nothing reaches is never compiled, so a construct the VM
  refuses inside one goes unreported — the diagnostic arrives only if
  something calls it (`tests/run/unreachable_body.dt`). The checker is
  unaffected; this is codegen only
- overlapping method names across impls: bare generic paths take the first
  registered impl
- `Point::new` vs `Point::<Int>::new`: expression paths require turbofish
- an associated-type projection cannot key an instantiation **through a
  bound**: handing a `T.Item` / `Self.Item` value to another generic
  (`id(v.item())`, or `it.next().unwrap()` where the payload is `I.Item`) reports
  "cannot instantiate 'id': type argument 'T' is not known here", because codegen
  has no `infer_apply` to collapse the projection once the base is concrete —
  `subst_apply` passes `TY_ASSOC` through untouched and `impl_index_assoc_type`
  is checker-side. Affects bounded generic functions and trait default bodies
  alike. The *checker* collapses these projections wherever it reads them —
  including, since milestone 47, a later argument's hint once an earlier argument
  pins the base, and a `for`-loop's element type — so a projection only bites at
  codegen when it must key a monomorphisation. Through a *trait object* it works
  (milestone 27): `trait_project` collapses `Self.Item` against the binding the
  `dyn` names, so codegen only ever sees a concrete type
- an impl's own type-param bounds are checked at selection (milestone 20), but
  *coherence* is deliberately blind to them: `impl<T: Ord> Ord for Option<T>`
  and an `impl Ord for Option<Widget>` conflict even though no receiver could
  ever satisfy both. Disjointness would have to be decidable to do better
- a self-referential blanket impl (`impl<T: Foo> Foo for T`) is cut off by
  `IMPL_BOUND_MAX_DEPTH` rather than diagnosed, so it silently fails to apply
  and the receiver reports "no method named 'foo'"
- two impls of one trait for one type can still coexist in a program, as long
  as no single module sees both — and if two such modules instantiate the same
  *generic* at the same type, `mono_request` memoises one copy and whichever
  requested it first decides the body. Keying instances on the visible set as
  well would fix it, at the cost of a copy per module. Non-generic bodies are
  not affected: since milestone 21 they compile against their own module's
  impl set, which no requester can change
- an `@intrinsic` cannot be used as a value (it is an opcode, so there is no
  body for a global slot to address) — reported at codegen, unlike the
  `@native` beside it which is fully first-class
- `@native`/`@intrinsic` on a *trait*-declaration method is still rejected — its
  default body is generic over `Self`, with no concrete C body to bind. An impl
  method may be native as of milestone 39, and since milestone 40 the standard
  library uses it: the primitive modules spell their operations as methods
  (`s.len()`, `xs.push(v)`, `c.code()`). What stays a free function is the
  design-forced exceptions — the `pad_*`/`float` lang items, the
  `join`/`concat`/`from_chars` builders (receiver is a collection), `print`
  (general over any `T`), `array`'s private `pop_last`, and the length
  `std::cmp` needs but cannot reach as a method without closing the
  `array → option → cmp` cycle (a method needs its impl visible; a free
  `@intrinsic` does not)
- shipping an inherent method on a primitive widely (`impl String`, `impl<T> [T]`,
  `impl Char` since milestone 40, and a *second* `impl String` in `std::text`
  since milestone 41) means a program importing that module cannot add its own
  inherent method of the same name — overlapping inherent methods are silently
  first-wins, with no coherence check, so it shadows rather than errors. The
  `Display`/`Ord`-for-containers cost, one level over, minus the diagnostic.
  Note this is only a problem *across* impls: two std impls for `String` coexist
  fine because their names are disjoint, which is what milestone 41 relies on
- a native's C signature is not checked against its ducktape one — the registry
  knows only "n values in, one out", so a mismatch is a std bug that the
  checker cannot catch
- an array grows but never shrinks its buffer: `pop` and `clear` leave the
  capacity where it got to, and it is released only when the array is collected
- `for x in xs` re-reads the length each iteration, so pushing to the array
  being iterated extends the loop instead of iterating a snapshot. There is no
  borrow checker to forbid it, and nothing diagnoses it
- `std::array` is no longer a leaf: `pop` returns an `Option`, so importing any
  of it reaches `std::option` and, transitively, every impl `std::fmt` and
  `std::cmp` ship. A program that wanted `push` and its own `impl Display for
  Int` cannot have both. Milestones 25 and 26 lengthened that chain — `std::cmp`
  now reaches `std::string` and `std::char`, and `std::char` reaches
  `std::panic` — but not its cost, since none of the three ships an impl to
  inherit
- `std::text::find` slices and interns a candidate substring at every position,
  so `find`/`contains`/`split` are O(*n·m*) allocations. There is no `byte_at`,
  so a window has to be cut out to be compared; a real substring search would
  need one, which is the byte-level reader `std::char` declined to offer
- `std::text::parse_int` wraps on overflow rather than failing: `acc * 10 + d`
  is an ordinary `Int` multiply, and detecting the wrap needs a widening multiply
  or a divide-back check the module has not been given a reason to write. So the
  `Option` it returns distinguishes "not a number" from a number, but not a
  number too large from one that fits
- `String` ordering is raw bytes: no locale, no case-insensitive compare, no
  Unicode normalisation, so `"Zebra"` sorts before `"apple"` and two strings
  that are canonically equivalent are simply different. Since milestone 26 a
  case-insensitive compare *is* expressible — `chars` plus `std::char::to_lower`
  — but only for ASCII, so what is left is a data problem (the case-mapping
  table) rather than a language one
- `std::char`'s classifications and case conversions are **ASCII-only**:
  `is_alpha('é')` is false, `to_upper('é')` is unchanged, and nothing warns.
  Full Unicode case mapping is a table rather than a range test, and there is no
  way to ship a partial one that is not silently wrong for most of the world
- a `String` is a byte string, not guaranteed valid UTF-8: `slice` cuts at byte
  offsets, so it can halve a multi-byte sequence, and `chars` is a runtime error
  on the result. Making the type carry the guarantee would mean validating every
  `slice`, which is the cost the byte-indexed API exists to avoid
- there is no way to get a `Char` out of a String without building the whole
  `[Char]`: `chars` is the only reader, so testing the first character of a long
  string allocates an array as long as it. A lazy or indexed form would need the
  byte/character question answered differently than milestone 26 answered it
- `Ord` ships for `Int`, `Float`, `Char`, `String`, `Option<T>`, `[T]` and
  `(A, B)` (the last two as of milestone 36), so an array of strings sorts and a
  tuple compares field by field without a per-program impl. As with `Display`,
  shipping them takes the pair away from a program that would write its own, and
  only the two-element tuple is covered — nothing is generic over a tuple's
  arity, so a `(A, B, C)` still has no order
- `Ord` for `Float` was IEEE comparison, so `NaN.cmp(x)` answered 0 for every
  `x` and NaN compared equal to everything. Fixed in milestone 42: the impl now
  decides a total order — NaN sorts after every real number and all NaNs are
  equal — with `self != self` as the NaN test, so `max(nan, x)` is NaN whichever
  side the NaN is and a `[Float]` with a NaN has a defined sort. What the
  placement ignores is the sign bit and payload: `-NaN` and `+NaN` are one
  order, unlike Rust's `total_cmp`, since nothing has needed to tell them apart.
  `String` never had this problem: every byte string is ordered against every
  other
- a `StringBuf` grows but never shrinks its *buffer*: `b.clear()` drops the
  length to zero so one buffer can be reused across iterations, but the capacity
  it grew to is kept, and released only when the buffer is collected
- a `StringBuf` can be appended to from a `String`, a `Char` (milestone 26) or an
  `Int`'s digits (`b.push_int(n)`, no `"{n}"` interned to carry them), but not
  from a *slice* of a String, so a `b.push_slice(s, from, to)` that avoids
  interning the window first is the natural next entry; it has not been needed yet
- a panic message can only name the value that caused it where the type
  parameter is bounded: `"{e}"` needs `E: Display`, and `Option`/`Result`'s
  `unwrap` are not bounded, so their messages stay fixed. `expect` is the way
  round it
- formatting is the corner of std the compiler knows by name, and since
  milestone 35 that is five names, not one: `Display` (captured in
  `tc_register_trait`) plus `pad_start`/`pad_end`/`pad_center`/`float` (captured
  in `tc_register_fun`), all keyed by module. Every other std item is anonymous
  to the compiler, so these are the exceptions to "the std/not-std difference is
  one branch in `mod_parse`" — each forced by the same rule, that a construct the
  compiler desugars (interpolation, a spec) cannot have its meaning depend on
  what the user happened to import
- `Never` is not *checked* to diverge: unification lets it stand in for any
  type in both directions, so `fun f() -> Never { return 1; }` is accepted and
  a `var x: Never` annotation is legal. It is a promise the compiler takes on
  trust, which is fine while `std::panic` is the only thing making it
- a panic does not unwind — no `catch`, and no way for a program to observe one
  and keep running. `Never` is only a *type*; the runtime behaviour behind it is
  "print the frames and stop"
- a trait object cannot be made from the abstract `Self` of a default body:
  `check_coerce_dyn` refuses a `TY_TRAIT`, so `self` inside a default body
  can't be handed on as a `dyn Trait` even when the trait is object-safe
- no downcast from `dyn Trait` back to a concrete type
- a trait's type arguments are never *inferred* at the place the trait is
  named: an impl head, a bound and a `dyn` each write them out, and a bare
  `Into` is an arity error rather than a request to work it out. The one
  exception is a `dyn` whose argument is an unsolved unknown, where the impl
  decides — and that is ambiguous the moment two impls answer
- **a bare method call on a type implementing one generic trait twice** picks
  by the *expected* type, and by first-registered-impl when there is none. So
  `print(c.into())` is not the same question as `var f: Fahrenheit =
  c.into()`, and only the second has an answer *as a bare call*. Since milestone
  31 the trait-qualified spelling `Into::<Fahrenheit>::into(c)` settles it
  explicitly, so the gap is now only the *bare* form's — a value context with no
  expected type has a way out, it is just not `c.into()`. Since milestone 29 the
  expected type also *pins* an impl parameter the receiver cannot reach, which
  sharpens the bare failure rather than removing it: where that was the only way
  to pin it, no impl applies at all and the diagnostic is "no method named 'into'"
- impl selection reads an **argument** type only through the qualified
  spelling: `Meters::from(x)` picks by `x` (milestone 30), and the receiver side
  `Into::<U>::into(x)` picks by `x`'s type (milestone 31), but only through those
  written forms. The argument (or receiver) must type hint-free to do so, so
  `Meters::from(None)` — where the argument is what would choose the impl yet
  needs one chosen to type — cannot be disambiguated
- a bound may name an earlier type parameter of the same list
  (`<T, U: Into<T>>`) but not a later one — bounds resolve left to right, so a
  forward reference is "unknown type: T" rather than a second pass
- a *generic* trait's parameters cannot themselves be pinned by the receiver
  in an impl head: `impl<T> Into<T> for S` type-checks, but nothing solves `T`
  from an `S`, so the impl applies only where a bound, a `dyn`, or (since
  milestone 29) the *expected type* names the argument. The last is what makes
  the `From`/`Into` blanket work, and it is the only one that is inference
  rather than a written-down reference
- the slot spaces are two bytes wide, so 65536 functions/structs/enums/vtables
  is a hard program-wide ceiling (reported, not silently truncated) — and every
  instantiation of a generic and every (trait, type) pair coerced spends one,
  so it is reached by *use* rather than by how much is written
- an unsupported construct inside a trait method is reported at the coercion
  site that builds the vtable, since that is where the body is first demanded
  — the same "only seen where it is instantiated" limitation generics have
- no glob `use a::*`. Module-qualified paths exist as of milestone 33 (`use
  a::b;` binds `b`, then `b::thing`), but a *glob* still has no spelling, and a
  `pub use` re-export names one item at a time — a façade module still lists
  them, and a qualifier is not re-exportable (it is not an item)
- `pub` is ignored on the `impl` keyword and rejected on a method or a struct
  field (see above); there is no sub-module visibility, so `pub` means something
  only on a top-level item, at module granularity
- module dedup is lexical, so one file reached by two different spellings
  (a symlink, say) would load twice and collide
- a bytecode image is structurally validated (bounds, indices, counts) but the
  code itself is not verified: an image with a plausible header and nonsense
  instructions can still crash the VM
- an image carries no source, so a runtime error in one reports function names
  with no line information
- `make sanitize` is a separate run, not part of `make test`: a sanitizer
  finding is not a test failure, so the suite stays green while the binary is
  doing something undefined. Nothing enforces that it is run before a commit
