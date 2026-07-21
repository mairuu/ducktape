# ducktape — language reference (as implemented)

Every example below compiles today; most are lifted from `tests/`. The grammar
in EBNF form is `references/grammar.ebnf`. Anything not listed here is either
in the "Not yet implemented" table at the bottom or does not exist.

Comments run from `#` to end of line. The test harness assigns meaning to two
prefixes: `#! expect:` (first line of a `tests/fail` file) and `#>` (expected
stdout in `tests/run` files) — the language itself treats them as comments.

## Declarations

```
use std::io::print;                  # `std::` is the builtin namespace
use geometry::Point;                 # loads geometry.dt — see "Modules"

fun add(a: Int, b: Int) -> Int {     # return type via ->, omitted = ()
    a + b                            # last expression is the return value
}

pub struct Point<T> { x: T, y: T, }  # named struct; pub = visible to importers
struct Pair(Int, Int)                # tuple struct
struct Unit;                         # unit struct

enum Result<T, E> { Ok(T), Err(E), }

trait Drawable {                     # item signatures are resolved & checked
    type Color;                      # against impls (conformance)
    fun draw(self) -> Self.Color;    # `Self.Color` = abstract projection
    fun visible(self) -> Bool { return true; }  # default; impls may omit it
}

impl Drawable for Point<Int> { ... } # trait impl
impl<T> Box<T> { ... }               # generic inherent impl
impl Point<Int> {
    type Color = String;             # associated type
    fun new(x: Int, y: Int) -> Self { Point { x: x, y: y } }
}
```

`var` at file scope parses but **aborts the compiler** (registration has no
case for it) — declare variables inside functions only.

## Types

| Syntax | Meaning |
|---|---|
| `Int` `Float` `Bool` `String` | primitives |
| `()` | unit |
| `(A, B)` | tuple |
| `[T]` | array of `T` |
| `fun(A, B) -> R` | function type |
| `Point<Int>` | generic instance |
| `dyn Drawable` | trait object — see "Trait objects" |
| `Self`, `Self.Color`, `Point.Color` | self type, associated types |

## Statements and blocks

```
var x = 1;                    # inferred; no `let`, no `mut` — all vars mutable
var y: [Int] = [1, 2, 3];     # annotated
var (a, b) = (1, 2.5);        # destructuring — the binding is a pattern
var Point { x: px, y } = p;   # ... any irrefutable one, nested freely
x = 2;  x += 1;               # assignment / compound assignment (+= -= *= /=)
return expr;  return;         # bare return means ()
break;  continue;             # inside loops only
```

Blocks are expressions: the trailing expression without `;` is the block's
value; otherwise the block is `()`. A trailing block-form `if`/`match` counts
as the tail (`fun abs(n: Int) -> Int { if n < 0 { -n } else { n } }`). A block
ending in `return` has type `!` (never), which unifies with anything.

## Expressions

- Number literals: `12`, `2.5`, and an exponent form that is a `Float` with or
  without a point (`1e-7`, `3E2`, `1.5e+10`). `1e` with no digits after it is
  the `Int` `1` followed by the identifier `e`.
- Arithmetic `+ - * / %` on numerics; `Int op Float` widens to `Float`.
  `+` also concatenates `String`s.
- Comparison `< <= > >=` (numeric), `== !=` (same static type).
- Logic: keywords `and`, `or` (short-circuit), `not`. There is no `&&`/`||`.
- Unary minus `-x` (numeric).
- `if cond { .. } else { .. }` is an expression; without `else` it is `()`.
- `while cond { .. }` and `for x in iter { .. }` evaluate to `()`;
  `iter` may be an array or a range.
- Ranges: `a..b`, `a..=b` — Int-only, first-class values (`var r = 0..10;`).
- Casts: `x as T` — only `Int`↔`Float` and identity casts.
- String interpolation: `"x = {x}"` — segments must be Int/Float/Bool/String.
- Calls: `f(a, b)`; functions are first-class (`var g = f; g(1)`).
- Closures use pipes: `|x, y| => x + y`, `|n: Int| -> Int { return n * 2; }`.
  Unannotated params infer from a function-typed hint
  (`var f: fun(Int) -> Int = |x| => x + 1;`) or from use; a closure whose
  types can't be pinned down is an error. `break` cannot escape a closure.
- A unit struct is a value under its bare name: `struct Marker;` then
  `var m = Marker;`, no `{}` suffix. The name is looked up in the value scope
  first, then as a zero-field struct.
- Paths: `Result::Ok(5)`, `Point::new(..)`. Explicit type arguments use
  turbofish in expression position: `Point::<Int>::new(1, 2)`. A bare
  `Point::new(..)` on a generic type selects the impl by method name and
  infers the type arguments from the call (first name match wins if several
  impls define the same name).
- Method calls: `p.draw()`, `p.x`, tuple access `t.0`. A method an impl
  omitted but whose trait gives a default body is inherited: the call checks
  against the trait's signature, projected into the impl's terms, and runs a
  copy of the body compiled for that receiver type. Inside such a body `Self`
  is a type parameter bounded by the trait — usable in annotations, argument
  and return position — so calls on `self` see exactly the trait's own
  methods, whichever impl ends up inheriting it.
- Trait bounds on type parameters are written inline (`fun f<T: A + B>(..)`),
  in a `where` clause (`fun f<T>(..) -> R where T: B`), or both — they merge.
  A bound must name a trait. Bounds are *enforced*: instantiating a bounded
  parameter with a type that has no `impl Trait for T` is an error
  ("type 'Int' does not implement trait 'Named'"), reported at the call site
  once inference has solved the parameter — including for an explicitly
  written type argument (`need_a::<Q>(q)`). Inside the generic body the bound
  *is* what makes the trait's methods callable: `t.draw()` on a `T: Drawable`
  resolves against the bound's signature (first bound declaring the name
  wins), and `T.Color` names an associated type through it. Both are abstract
  until instantiation — the projection collapses to whatever the applicable
  impl bound it to once T is solved (`tests/pass/trait_bound_calls.dt`).
  A type parameter also *satisfies* the bounds it was declared with, so a
  bounded generic can hand its parameter to another one that requires the same
  trait (`tests/run/generic_impls.dt`).
- Generic code runs: each generic function, method and impl is compiled once
  per distinct tuple of type arguments, discovered from its call sites
  (`runtime.md` "Monomorphisation"). A generic definition nobody calls is
  never compiled and is not an error.

### Trait objects

`dyn Trait` is a value that carries its own implementation, so a collection
can hold several concrete types at once:

```
var shapes: [dyn Shape] = [Sq { s: 3 }, Rect { w: 2, h: 5 }];
for s in shapes { print(s.area()); }        # dispatched per element
```

The `dyn` keyword is **required**. A bare trait name in type position stays
the *bound* spelling (`impl Shape for Sq`, `where T: Shape`) — the two are
different dispatch strategies, so the source says which one it means.

A concrete value **coerces** to `dyn Trait` where one is expected: as a call
argument, a `return` value, a `var` initializer with a `dyn` annotation, a
struct or enum-variant field initializer, or an element of a `[dyn T]` or
`(dyn T, ..)` literal. This is the language's only
subtyping, and it is one-way and non-transitive — a `dyn Shape` is not a `Sq`
again, and there is no cast back. The coercion needs an `impl Trait for T` to
exist, the same question a bound asks; without one the type error is the
ordinary "expected 'dyn Shape' but got 'Circle'". A bounded type parameter
coerces too, so a generic function can hand its own parameter over.

Not every trait can be one. A trait is **object-safe** only if every method
takes `self`, has no type parameters of its own, and does not mention `Self`
outside the receiver (so no `-> Self`, no `other: Self`, no `Self.Item`) — a
trait object has erased the concrete type, and those signatures all need it
back. The rule is checked where `dyn Trait` is *written*, not at the trait
declaration, so a trait may freely be static-dispatch-only and still be used
as a bound (`tests/run/trait_default.dt` leans on all three).

A default body works through a trait object like any other method, and an
impl that overrides it wins, exactly as under static dispatch. Printing and
`==` see through the wrapper — a `dyn Shape` over a `Sq` prints as the `Sq`.
See `tests/run/trait_objects.dt`, and `runtime.md` "Trait objects" for the
vtable representation.

### match

```
var m = match p {
    Point { x: 10, y: val } if val > 0 => true,   # guard sees bindings
    _ => false,
};
```

Patterns: literals, `_`, bindings, tuples `(a, b)`, variants
`Result::Ok(v)` / `Status::Active`, structs `Point { x, y }` (including
literal sub-patterns `x: 10`). A tuple struct is written with its constructor
spelling, `Pair(a, b)`, in a pattern as in an expression, and so is a unit
struct: a bare `Marker` is a test for that struct, not a binding. The cost is
that a unit struct's name can no longer be bound as a variable — `var Marker =
7;` is a struct pattern against an `Int` and is rejected.

The same patterns are the binding form of `var`, restricted to the
irrefutable ones — a `var` binding is a match with one arm and no guard, so
"irrefutable" is decided by the exhaustiveness checker below. `var Opt::Some(n)
= o;` is rejected (`refutable pattern in a 'var' binding`) because `Opt::None`
would reach no arm; use `match`.

Matches must be **exhaustive**; a gap is a compile error naming the missing
enum variant where it can (`match is not exhaustive: 'Shape::Point' is not
covered`). A guarded arm never counts towards coverage — whether it matches is
a runtime question. Types with no enumerable domain (`Int`, `Float`, `String`)
therefore always need a `_` or a binding arm.

### `?` propagation

`expr?` is structural: the operand must be an enum with exactly two
single-field tuple variants named `Ok` and `Err`, and the enclosing function
(or closure) must return the *same* enum; the `Err` payload types must unify.
The result is the `Ok` payload. There is no built-in `Result` — declare your
own (`tests/pass/propagate.dt`). This rule will move to a `Try` trait once
traits are fully checked.

## Modules

A program is one root `.dt` file plus every file reachable from it through
`use`. There is no `mod` keyword: `use` is what pulls a file in.

```
use geometry::Point;                 # <root_dir>/geometry.dt
use geo::point::{Point, Line};       # <root_dir>/geo/point.dt, two items
use util::Maybe as Opt;              # imported under a different name
```

The leading segments name the *file*; the final segment (or the brace group)
names the *items*. Paths are relative to the directory of the root file — the
one search root — for every module, including modules that are not the root.
A file reached by two different importers is loaded once.

Items are **private by default**; `pub` makes one importable:

```
pub struct Point { x: Int, y: Int }  # importable
fun helper() -> Int { 1 }            # private: `use m::helper;` is an error
```

Imports are **not** transitive. If `b.dt` does `use a::X;`, that does not
re-export `X` — `use b::X;` from a third module is an error. There is no
`pub use`.

A path is never module-qualified: once imported, an item is named directly
(`Point`, not `geometry::Point`). Trait `impl`s are the exception to all of
this — they are program-wide and apply wherever their type is used, whether or
not the defining module was imported.

A cycle in the import graph is an error, reported with the chain of modules
involved.

## The standard library

`std::` names the standard library, which is **written in ducktape** — the
`.dt` sources live in `std/` and are mirrored into the compiler binary, so
`use std::cmp;` needs no install path, environment variable or search
directory. `std::` never touches the filesystem, so a local `std.dt` is
unreachable.

```
use std::cmp::{Ord, max, min, clamp};
```

A std module is an ordinary module in every other respect: it is deduplicated,
takes part in the dependency graph, honours `pub`, and is loaded **only if
some `use` names it** — a program that ignores std pays nothing. Naming a std
module that does not exist is an error listing the ones that do.

### `std::cmp`

```
pub trait Ord {
    fun cmp(self, other: Self) -> Int;      # <0 before, 0 equal, >0 after
    fun lt(self, other: Self) -> Bool       # defaults, derived from cmp
    fun gt / le / ge
}

pub fun max<T: Ord>(a: T, b: T) -> T
pub fun min<T: Ord>(a: T, b: T) -> T
pub fun clamp<T: Ord>(v: T, lo: T, hi: T) -> T
```

`Ord` is implemented for `Int` and `Float`, which is the point worth noticing:
**a trait can be implemented for a primitive**, so the standard library extends
the built-in types with exactly the machinery user code uses. Your own types
join the same trait and `max`/`min` work on them unchanged
(`tests/run/std_cmp.dt`).

`Ord` is deliberately *not* object-safe — `other: Self` means a caller must
know the concrete type — so it is a bound, never a `dyn Ord`. That is the rule
working as designed: object safety is only demanded where `dyn` is written.

## Built-ins

`print<T>(value)` — prints any value followed by a newline, returns `()`.
Registered in every module; the only builtin so far, and the only thing that
does not need importing.

A `Float` prints — and interpolates — as the shortest decimal that reads back
as the same double, always carrying a `.` or an exponent so it is never
mistaken for an `Int`: `1.0`, `0.3333333333333333`, `300.0`, `1e+18`, `1e-07`,
`-0.0`, `inf`, `-inf`, `NaN`. Exponent form takes over below `1e-5` and at
`1e17`; every form printed is also a literal the scanner accepts.

`use std::io::print;` is accepted and does nothing — it predates the embedded
library and names a builtin that is already in scope. It is the one `std::`
path with no module behind it, and goes away when `print` becomes a real std
module.

## Not yet implemented

| Gap | Behavior today |
|---|---|
| extra (non-trait) methods in a trait impl | tolerated as inherent methods (Rust rejects them) |
| `dyn Trait` for a non-object-safe trait | rejected where `dyn` is written, naming the method and the reason |
| coercing the abstract `Self` of a default body to `dyn Trait` | not offered — `self` inside a default body cannot be handed over as a trait object |
| casting a `dyn Trait` back to its concrete type | no downcast; the coercion is one-way |
| `dyn Trait` with type arguments (`dyn Into<Int>`) | generic traits are not parameterised in `dyn` position |
| `mod` declarations | no such keyword; `use` is what pulls a file in |
| `pub use` re-export | imports are not transitive; a third module can't reach through an importer |
| glob imports (`use a::*`) | not parsed; name each item |
| module-qualified paths (`geometry::Point`) | import the item and name it directly; the diagnostic says so |
| `pub` on impls, methods, and fields | parsed and ignored — a public type's fields and methods are all visible |
| visibility below module granularity (`pub(crate)` &c.) | `pub` is the only modifier |
| two spellings of one file (symlinks, unusual paths) | dedup is lexical, so the file would load twice and collide |
| top-level `var` (globals) | parses, then a registration diagnostic: move it into a function |
| tuple-struct struct-patterns `Pair { a, b }` | write the constructor spelling `Pair(a, b)` — "matching tuple struct with struct pattern syntax is not allowed" |
| variable shadowing diagnostics | a `var` may silently shadow an earlier one in the same scope (top-level *item* names do collide — that is an error) |
| overlapping method names across impls of one type | bare paths pick the first registered impl |
| capturing a `for` loop variable in a closure | runs, but the closure sees the loop variable's *final* value (one shared cell), not a per-iteration copy — `runtime.md` "Closures & upvalues" |
| infinitely deep generic instantiation | `fun grow<T>(v: T) { grow([v]) }` type-checks but names a new instantiation at every level; codegen stops at 32 and reports it (`runtime.md` "Monomorphisation") |
| more than 256 functions, counting one per instantiation | each instantiation takes a global slot, so a heavily generic program can outgrow the one-byte operand space |
| `print` named as a value (`var p = print;`) | a call lowers to `OP_PRINT`, so the builtin has no body to point a slot at — "using a builtin as a value is not supported by the VM yet" |
| generic `main` | nothing calls the entry point, so no instance is ever made — "'main' must not be generic" |
