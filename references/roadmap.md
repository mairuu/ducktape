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

## Next (in recommended order)

Estimates are relative to one focused session ≈ the checker-completion
milestone (~900 lines).

Nothing on the main line is *blocked*: every construct the checker accepts in
`tests/pass/in_fixed.dt` now also runs. The list below is what the "known
warts" section would promote first, in the order that pays off soonest — pick
by appetite rather than by necessity.

1. **A formatting story** — now the most-requested thing the library cannot
   express, and it blocks three separate items. Interpolation is defined over
   primitives, so a generic value cannot be printed: `Result::unwrap` panics
   without naming the error, `Float` has no precision or width, and no user
   type can decide how it renders. `value_print` already walks every value
   shape, so the mechanism is a sink abstraction over it plus a
   `to_string<T>(v: T) -> String` native — the design question is whether a
   user type overrides it through a `Display` trait, which is the part that
   needs a decision rather than code.
2. **Module-granular impls and `pub use`** — `ImplIndex` lives on the
   `TypeChecker`, so an impl applies even where its module was never
   imported, and imports don't compose (no re-export, no glob, no qualified
   paths). Now also the reason a user's `impl Ord for Int` collides with
   `std::cmp`'s. Fixing the first is a scoping change; the second is parser
   plus `tc_link_imports`.
3. **Growing std on top of the natives** — the mechanism landed in milestone
   16 with a deliberately small registry (`print`, array `len`, string
   `len`/`slice`). What it does not yet reach is anything that *mutates* or
   *grows* a heap object: `push` needs `ObjArray` to stop being fixed-size,
   which is a real allocator question rather than another table entry. String
   comparison, `to_string` with a width or a precision, and a `Char` type are
   each one entry plus a decision about the type they need.
4. **Wider slot spaces** — 256 functions/structs/enums program-wide, now
   reached by *use* of generics rather than by how many are written, by
   trait-object vtables on top of that, and by natives once they take slots
   too — three independent pressures on one byte. A two-byte operand (or a
   wide-operand opcode pair) lifts it.

   Worth weighing first, though: **reachability-based linking** may buy more
   for less. `exe_link` currently gives every non-generic definition a slot
   whether or not anything calls it, which is why importing `std::cmp` spends
   two slots on `Int::cmp`/`Float::cmp` even when only one is used — and why a
   growing std taxes every program that touches it. Generic definitions
   already behave the right way (an uncalled one is never compiled and costs
   nothing), so making non-generics match is the *consistent* fix rather than
   a new mechanism. It needs a reachability walk from `main` through calls,
   vtables, closures and `?`, which is real work — but it shrinks the pressure
   instead of just moving the ceiling.
5. **Object-safe traits with associated types** — the object-safety rule
   rejects `Self.Item` outright. Allowing `dyn Iterator` with the associated
   type *named* at the coercion site (`dyn Iterator<Item = Int>`) is the
   natural follow-on, and needs `dyn` to carry type arguments at all.

Not on the roadmap: the **REPL** is a side feature, not a milestone — it lives
on the `feature/repl` branch (`--repl`, incremental compilation over one module
via `Module.decl_base`) and is not part of the main line.

## Known warts to clean up opportunistically

- a `Float` still has no user-facing formatting control: `value_format_float`
  picks the shortest round-tripping decimal and that is all there is. A
  `to_string` with a precision or a width needs a real formatting story
  (`runtime.md` "Values") — now a native away, but still needing the story
- a unit struct's name cannot be bound as a variable any more: `var Marker =
  7;` is a struct pattern against an `Int`, and the diagnostic ("expected
  struct type in struct pattern") describes the rewrite rather than the
  mistake. Rust behaves the same way; the message could be kinder
- no shadowing diagnostics for `var` (`vscope_define` todo); top-level item
  names do collide, but a `var` may silently shadow one in the same scope
- a refutable `var` binding whose column type inference never pinned down is
  accepted (the tri-state answer reports nothing) and traps at runtime via
  `OP_MATCH_FAIL` instead of at compile time
- `pub` is rejected outright on an impl item (`pub fun` inside an `impl`
  block is "expected impl item"), though it is parsed and ignored on a struct
  field. Method visibility is not a thing, so std impls simply omit it
- overlapping method names across impls: bare generic paths take the first
  registered impl
- `Point::new` vs `Point::<Int>::new`: expression paths require turbofish
- every instantiation takes a global slot, so the 256-function ceiling is now
  reached by *use* of generics, not just by how many are written
- a generic definition's diagnostics are only seen where it is instantiated,
  so an unsupported construct inside an uncalled generic goes unreported
- an associated-type projection cannot key an instantiation: handing a
  `T.Item` / `Self.Item` value to another generic (`id(v.item())`) reports
  "cannot instantiate 'id': type argument 'T' is not known here", because
  codegen has no `infer_apply` to collapse the projection once the base is
  concrete — `subst_apply` passes `TY_ASSOC` through untouched and
  `impl_index_assoc_type` is checker-side. Affects bounded generic functions
  and trait default bodies alike
- trait impls are program-global: an impl applies even if its module was never
  imported (`ImplIndex` lives on the `TypeChecker`, not the `Module`) — which
  also means a user's `impl Ord for Int` silently loses to `std::cmp`'s once
  that module is imported, first registration winning
- an `@intrinsic` cannot be used as a value (it is an opcode, so there is no
  body for a global slot to address) — reported at codegen, unlike the
  `@native` beside it which is fully first-class
- `@native`/`@intrinsic` are only accepted on a top-level `fun`: an impl method
  or a trait method cannot be native, so a primitive's operations have to be
  free functions (`string::len(s)`, not `s.len()`)
- a native's C signature is not checked against its ducktape one — the registry
  knows only "n values in, one out", so a mismatch is a std bug that the
  checker cannot catch
- `ObjArray` is fixed-size, so `std::array` has `len` and nothing that grows or
  mutates
- a panic message cannot name the value that caused it: interpolation is
  defined over primitives, so `"{e}"` on a generic `E` is an error and
  `unwrap`'s message is a fixed string. `expect` is the way round it
- `Never` is not *checked* to diverge: unification lets it stand in for any
  type in both directions, so `fun f() -> Never { return 1; }` is accepted and
  a `var x: Never` annotation is legal. It is a promise the compiler takes on
  trust, which is fine while `std::panic` is the only thing making it
- a panic does not unwind — no `catch`, and no way for a program to observe one
  and keep running. `Never` is only a *type*; the runtime behaviour behind it is
  "print the frames and stop"
- importing `std::cmp` spends two global slots on `Int::cmp`/`Float::cmp` even
  if only one is used — non-generic definitions are linked whether called or
  not, unlike the generics around them
- a trait object cannot be made from the abstract `Self` of a default body:
  `check_coerce_dyn` refuses a `TY_TRAIT`, so `self` inside a default body
  can't be handed on as a `dyn Trait` even when the trait is object-safe
- no downcast from `dyn Trait` back to a concrete type, and no `dyn` with type
  arguments (`dyn Into<Int>`) — a generic trait can only be named bare
- every distinct (trait, concrete type) pair a program coerces takes a vtable
  slot out of the same 256-wide operand space as everything else
- an unsupported construct inside a trait method is reported at the coercion
  site that builds the vtable, since that is where the body is first demanded
  — the same "only seen where it is instantiated" limitation generics have
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
