# The declared module tree — design

Status: **§1–§3, §5 and §7 shipped in milestone 94**; `language.md` and
`architecture.md` now describe them and are the place to read. What is left is
**§4 (privacy), §6 (the diagnostics the boundary makes possible) and the std
half of §5 — milestone 95**, after which this file is deleted.

Milestone 94 departed from the text in three places, all recorded in its commit
body: discovery is a fixpoint rather than a pass; the build unit is per tree
(the program's registered whole, the library's on demand) rather than uniform;
and `use` naming a module's own child is exempted from the §2.1 collision, since
`mod` and `use` are different edges and a module may want both.

Written against `54426f4` (milestone 93). The measurements in §1 and §7 were
taken at `2c0071c`, before milestones 92–93 added the unused warnings and
`@allow`; nothing those two changed touches resolution, and §6.1 records where
they meet this design.

## 1. Why

Three failures, each reproduced against `2c0071c`:

1. **A path's meaning depends on which files exist.** With `a.dt` exporting
   `b`, `use a::b;` imports that item. Creating an unrelated `a/b.dt` makes the
   same unchanged source mean *module `a::b`*, and it stops compiling.
2. **The fallback chain is reachable by accident.** `use Event::A;` naming the
   importing file's *own* enum breaks the moment an unrelated `Event.dt`
   appears, because a scope import is "the last reading, taken only where no
   file answered".
3. **There is no build unit.** A `.dt` file with a type error that nothing
   imports is never compiled. Nothing declares what belongs to the program.

Structurally: resolution *probes* (`mod_prefix_exists` at `count`, `count-1`,
`count-2`, with "shortest prefix tried last" as a tiebreak convention rather
than a rule); the answer is stored as flags the parser could not determine
(`is_module_import`, `is_scope_import`, `qualifier`); there are two resolvers —
filesystem and embedded table — that must be kept in agreement by hand, which is
what milestones 90 and 91 were both about; and there is no privacy, so every
file is permanent public API.

**One cause: the module tree is never declared, so every path re-derives it
from the filesystem.** Declaring it turns every question above into a lookup.

## 2. The model

A **module tree** per root. There are exactly two roots:

| root | named by | source of the root module | base directory |
| --- | --- | --- | --- |
| the program | nothing — its children are addressed directly | the entry path from `argv` | `dirname(entry)` |
| the library | the reserved first segment `std` | `std/lib.dt` | `std/` |

`std` is a **reserved** first segment: a program cannot have a child module
named `std`. This is today's behaviour (std is intercepted before the
filesystem) stated as a rule instead of an interception.

**Paths are absolute.** `use geo::shape::Rect;` reads from the program root
wherever it is written; `use std::cmp::max;` reads from the library root. There
is no `self`, no `super`, no `crate` — one anchor per root, and a path means the
same thing in every file.

### 2.1 Declaring a child

```
mod util;        # private child
pub mod geo;     # public child
```

A `mod` declaration is only valid at module level. The declared name is bound in
the declaring module's scope **alongside its items**, so `mod foo;` beside
`fun foo()` or `struct foo` is a collision error. That rule is what makes
resolution unambiguous without lookahead: a name in a module is a child module
or an item, never both.

### 2.2 Where a module's file lives

For a module `M`, let `path(M)` be its segments from the root (empty for a
root):

- **child directory** — `base_dir + join(path(M), "/") + "/"`.
  A root's children therefore live in the base directory itself, which is what
  every existing layout already does (`main.dt` beside `helper.dt`).
- **source file** — `child_dir(parent(M)) + name(M) + ".dt"`.
  A root's source is given rather than derived (the entry path; `std/lib.dt`).

So `mod geo;` in the root is `geo.dt` with children in `geo/`, and `pub mod
shape;` inside it is `geo/shape.dt`. **This is exactly the current filesystem
layout**, so migration adds declarations and moves no files — with one
exception, §7.

**A directory alone is not a module.** `geo/` is only meaningful because
`geo.dt` declares what is in it. This is what deletes the hand-written facade:
`std/collections.dt` stops mirroring `pub use` lines and becomes the file that
declares `pub mod hashmap;`.

### 2.3 `mod` and `use` are different edges

| | `mod x;` | `use a::b;` |
| --- | --- | --- |
| means | `x` is part of this build unit | these names are in scope here |
| causes | `x` is loaded, compiled and checked | a dependency edge; impl visibility |
| shape | a tree, acyclic by construction | a graph; cycles still diagnosed |

Declaring a child does **not** bring its impls — `use` still does, transitively,
exactly as today. Keeping the impl model untouched is deliberate: it is the one
part of the current system with no soundness problem.

The build unit is everything reachable from the root by `mod`. That is what
makes the orphan in §1.3 diagnosable.

## 3. Resolution

Given a use path `P` in module `M`, with `bare` (no braces) and `glob` flags:

```
1.  if P[0] == "std":  cur = std_root;      i = 1
    else:              cur = program_root;  i = 0

2.  while i < len(P) and cur declares a child named P[i]:
        cur = that child
        check §4 visibility of cur from M
        i += 1

3.  rest = len(P) - i

4.  if i == 0 and rest > 0 and P[0] names an enum in M's own type scope:
        -> scope import (`use Event::A;`); rest must be 1 or 2
        (only reachable for a program-rooted path: a `std::` path
         always consumed at least the root)

5.  otherwise `cur` is the module and `rest` is what follows it:

        bare,   rest == 0  -> module import: bind `cur` as a qualifier
        bare,   rest == 1  -> item `P[i]` of `cur`
        bare,   rest == 2  -> variant `P[i+1]` of enum `P[i]` of `cur`
        braced, rest == 0  -> the braced names are items of `cur`
        braced, rest == 1  -> the braced names are variants of enum `P[i]`
        glob,   rest == 1  -> every variant of enum `P[i]`
        anything else      -> error, naming what `cur` resolved to
```

Step 2 is greedy and cannot be ambiguous, because §2.1 forbids a name from being
both a child and an item. Nothing in this algorithm asks whether a file exists —
a missing file for a *declared* module is an error at tree construction (§6),
reported once against the `mod` declaration, not once per importer.

Step 4 is the only survivor of the old fallback chain, and it is now
deterministic: `Event` is a module only if some module declared it, and
declaring it in the same module that declares `enum Event` is already an error.

**No module glob.** `use a::*;` stays unsupported; `*` always names an enum's
variants. Nesting makes a module glob *implementable* — it would be the natural
way to write a facade — but it is the first thing in the language that would
bind names nobody wrote, so it stays a separate decision.

## 4. Privacy

A child declared `mod x;` is **private**; `pub mod x;` is public. Private means:
reachable from the declaring module and from every module in its subtree.

For a resolved module `T` reached from `M`, every component `C` on `T`'s path
from the root must satisfy: `C` is `pub`, **or** `parent(C)` is an ancestor-or-
self of `M`.

Item `pub` is unchanged and now composes: an item is reachable only if its
module is. This is what finally lets std hold internals — `std::array`'s raw
`pop_last`, `std::iter`'s `char_at`/`prev_boundary`, `std::string`'s
`char_width` are all documented as "std wants to keep this to itself" and are
all reachable today.

## 5. The library

`std/lib.dt` is the library root and the single statement of its public surface:

```
pub mod array;
pub mod cmp;
pub mod collections;
...
```

`std/collections.dt` becomes `pub mod hashmap; pub mod hashset;` plus whatever
`pub use` lines keep the short spelling working. `std/string.dt` gains
`pub mod buf;`.

The embedded table stops being a resolver and becomes a **loader**: the tree
comes from declarations, and `std_module_source(relative_path)` only answers
"give me the bytes". `embed_std.sh` is unchanged.

### 5.1 The lint hatch

Replaced by an explicit flag:

```
./build/ducktape --std-module std::cmp     # reads std/cmp.dt from disk
```

The module path says which module it is; nothing is inferred from the shape of a
filesystem path. The target's *dependencies* still come from the embedded table,
so the existing caveat stands: editing `std/lib.dt` and linting without `make`
first uses the previous tree.

This deletes `std_name_for_entry`, `mod_adopt_std`, `Module.alias_path` and the
`check_not_adopted`/`check_adopted` harness pairs — **milestone 90's entire
mechanism, and the half of milestone 91 that generalised it.** Plain
`./build/ducktape std/cmp.dt` becomes an ordinary program root and fails
predictably on `@lang`, with no adoption magic to explain.

## 6. Diagnostics the tree makes possible

Errors:

- a `mod` naming a file that does not exist — reported once, at the declaration,
  rather than once per importer
- `mod foo;` colliding with an item `foo` (§2.1)
- a private module named from outside its subtree, pointing at the `mod`
  declaration that would need `pub`
- the unknown-module note becomes a walk of the declared children of whatever
  `cur` reached, so it lists *siblings of the thing you got wrong* instead of
  every module in the library

One new warning, `orphan_module`: a `.dt` file in a **non-root** module's child
directory that no `mod` claims. Deliberately not applied to the base directory —
it is owned by nothing, and every single-file test in `tests/run/` is a sibling
of every other.

### 6.1 Where milestones 92–93 meet this

- **`mod` never warns as `unused_import`.** A `use` binds a name; a `mod`
  declares membership, and the name it binds is a consequence. An unclaimed
  module is a build-unit question, which `orphan_module` answers from the other
  direction — the file nothing declares, rather than the declaration nothing
  spends.
- **`orphan_module` joins the lint table** and is silenceable with
  `@allow("orphan_module")` on the declaring module, which is the escape hatch
  for a file kept beside a module on purpose.
- **Privacy is an error, not a lint.** `@allow` cannot reach it, deliberately: a
  private module named from outside is a fact about the program, not advice.

## 7. Migration

Measured against `2c0071c`.

| what | count | change |
| --- | --- | --- |
| multi-file test roots | 38 | add one `mod` per imported sibling to `main.dt` |
| non-root test modules | 54 | add `mod`/`pub mod` where they have children |
| directory modules with no file | 2 | `tests/pass/mod_nested/shapes.dt` and `tests/run/mod_linked/shapes.dt` are new, each declaring its one child |
| std root | 1 | `std/lib.dt` is new |
| std group modules | 2 | `collections.dt` rewritten as declarations; `string.dt` gains `pub mod buf;` |
| `use std::` lines | ~500 | **unchanged** — absolute paths keep their spelling |
| single-file tests | ~500 | **unchanged** — no `mod`, no siblings |

The ~500 `use std::` lines that made nesting urgent are untouched by this,
because §2 kept paths absolute. The migration is additive: no file moves, and
no import is rewritten.

## 8. Staging

**Milestone 94 — the tree.** `mod`/`pub mod` parsing; a tree-construction pass
between discovery and import collection; `mod_collect_imports` replaced by §3;
`mod_prefix_exists`, `std_module_prefix`, `is_scope_import`'s probing and the
`std_name_for_entry` family deleted; `--std-module`; the migration in §7. Ends
with all three failures in §1 fixed and the suite green. `pub mod` parses and is
recorded but is not yet *enforced*, so nothing a program can write changes
meaning.

**Milestone 95 — the boundary.** Privacy enforcement (§4), `orphan_module` and
missing-module diagnostics (§6), std's internals actually made private, and the
`pub use` facade lines dropped where `pub mod` replaces them.

Splitting there is deliberate: 94 is a mechanism change with a large mechanical
migration and no new *rules* for a user to learn, and 95 is the opposite. A
regression in 94 shows up as a resolution failure; one in 95 shows up as a
visibility failure. Keeping them apart keeps a bisect meaningful — and it means
the migration lands before the rules that would make a half-migrated tree fail
for a second reason.

## 9. Deliberately not in scope

- **A module glob** (`use a::*;`) — §3.
- **Relative paths** (`super::`, `self::`, `crate::`) — one anchor per root was
  chosen over expressiveness; revisit only if a real tree gets deep enough to
  hurt.
- **Changing the impl model.** Impl visibility stays reachability-based and
  transitive over `use`. `mod` grants nothing.
- **A manifest.** The declarations travel with the source, which the embedded
  library needs anyway.
- **Making the prelude a decision.** It is still a computed closure, and it is
  still ~82% of a trivial compile (0.8ms for a no-prelude root against 4.5ms for
  `fun main() {}`). That is a separate milestone and this design does not block
  it.
