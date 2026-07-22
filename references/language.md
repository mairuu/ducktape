# ducktape — language reference (as implemented)

Every example below compiles today; most are lifted from `tests/`. The grammar
in EBNF form is `references/grammar.ebnf`. Anything not listed here is either
in the "Not yet implemented" table at the bottom or does not exist.

Comments run from `#` to end of line. The test harness assigns meaning to two
prefixes: `#! expect:` (first line of a `tests/fail` file) and `#>` (expected
stdout in `tests/run` files) — the language itself treats them as comments.

## Declarations

```
use std::io::print;                  # `std::` is the embedded library
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
| `Never` | code that does not come back — see "`std::panic`" |
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
- String interpolation: `"x = {x}"` — a primitive segment (Int/Float/Bool/
  String) renders itself; anything else must implement `std::fmt::Display`.
- Calls: `f(a, b)`; functions are first-class (`var g = f; g(1)`).
- Closures use pipes: `|x, y| => x + y`, `|n: Int| -> Int { return n * 2; }`.
  Unannotated params infer from a function-typed hint
  (`var f: fun(Int) -> Int = |x| => x + 1;`) or from use; a closure whose
  types can't be pinned down is an error. `break` cannot escape a closure.
- A unit struct is a value under its bare name: `struct Marker;` then
  `var m = Marker;`, no `{}` suffix. The name is looked up in the value scope
  first, then as a zero-field struct.
- A constructor that carries no value to infer from — a unit variant
  (`Opt::None`) or a unit struct of a generic type (`Empty`) — takes its type
  arguments from the *expected* type: an annotation, a parameter type, the
  enclosing function's return type, a field, or an array/tuple element. With
  no expected type there is nothing to go on and it is an error asking for an
  annotation; a turbofish (`Opt::<Int>::None`) still overrides
  (`tests/run/unit_variant_infer.dt`).
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
The result is the `Ok` payload. Nothing about the rule is tied to a particular
enum, so you may declare your own (`tests/pass/propagate.dt`); `std::result`
declares the one that fits it. This rule will move to a `Try` trait once traits
are fully checked.

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

A plain `use` does not re-export. If `b.dt` does `use a::X;`, then `use b::X;`
from a third module is an error — "imported by module 'b.dt' but not
re-exported". Writing `pub use` makes the alias an item of `b` like any other:

```
# geo.dt
pub use inner::{Point, mk};   # Point and mk are now items of geo
use inner::hidden;            # geo's own business; not importable through geo
```

An importer never needs to know where a re-exported item was originally
written, which is what lets a module present a façade over the files behind it.

A path is never module-qualified: once imported, an item is named directly
(`Point`, not `geometry::Point`).

### Where an `impl` applies

An `impl` has no name, so it cannot be imported one item at a time.
**Reachability is the handle instead: an `impl` applies in a module if it is
written there, or in any module reachable from it through `use`,
transitively.** A module that never reaches the defining module does not see
the impl, and the diagnostic says so rather than merely reporting a missing
method:

```
error: no method named 'show' found for type 'S'
> note: an applicable impl exists in module 'owner.dt', but this module does
        not import it
```

Reachability is transitive because `pub use` can re-export a type whose impls
live one module further away.

Two implementations of **the same trait for overlapping types** may not be
visible at once. Writing `impl Ord for Int` in a module that also imports
`std::cmp` is an error, not a silent override:

```
error: conflicting implementations of trait 'Ord' for type 'Int'
> note: the other one is in module '<std>/cmp.dt'
```

Two modules that cannot see each other may each implement the same trait for
the same type; the conflict is reported at whatever `use` first makes both
visible. Inherent (`impl Point`) blocks never conflict — splitting a type's
methods across several is ordinary.

A cycle in the import graph is an error, reported with the chain of modules
involved.

## The standard library

`std::` names the standard library, which is **written in ducktape** — the
`.dt` sources live in `std/` and are mirrored into the compiler binary, so
`use std::cmp;` needs no install path, environment variable or search
directory. `std::` never touches the filesystem, so a local `std.dt` is
unreachable. Where ducktape cannot express the operation, a module declares a
bodyless function bound to C (see "Native functions" below); `std::cmp`,
`std::option` and `std::result` need none, `std::io` and `std::panic` are
nothing but. `std::fmt::Display` is the only std name the *compiler* knows.

**There is no prelude.** Every std name, `print` included, has to be imported.

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

### `std::fmt`

```
pub trait Display {
    fun to_string(self) -> String;
}

pub fun to_string<T: Display>(value: T) -> String
pub fun float(value: Float, precision: Int) -> String   # @native

impl Display for Int / Float / Bool / String
impl<T: Display> Display for [T]
impl<A: Display, B: Display> Display for (A, B)
```

`Display` is **the one standard-library name the compiler knows.** Every other
std item is anonymous to it — `Option` is an ordinary enum, `Ord` an ordinary
trait — but interpolation has to decide which `to_string` a `"{v}"` segment
means, and that cannot be left to whatever happens to be in scope.

So `"{v}"` splits in two:

- a **primitive** segment (Int, Float, Bool, String) renders itself. This is
  the path the VM has always had, and it is what lets a program interpolate a
  number without importing anything.
- **anything else** must implement `Display`, and the segment *is* the call
  `v.to_string()` — checked and compiled as exactly that. So every dispatch
  shape a method call already had works: a concrete impl, a `T: Display`
  bound, and a `dyn Display` vtable (`tests/run/fmt.dt`).

```
impl Display for Point {
    fun to_string(self) -> String { return "({self.x}, {self.y})"; }
}

print("{p}");                              # (1, 2)
fun label<T: Display>(v: T) -> String { return "[{v}]"; }
var xs: [dyn Display] = [p, Colour::Red];
```

Without the bound, `"{v}"` on a type parameter is an error naming the bound to
add. If no module in the program imports `std::fmt` at all, the diagnostic says
*that* instead, rather than reporting a failed bound that was never in play.

`Display` ships for the four primitives, so a `T: Display` generic can be
instantiated at one. Each impl is written by interpolating `self` — the
built-in path — which makes them free.

**The containers implement it too**, each requiring the same of its elements:

```
impl<T: Display> Display for [T]                 # [1, 2, 3]
impl<A: Display, B: Display> Display for (A, B)  # (1, hi)
impl<T: Display> Display for Option<T>           # Some(5) / None
impl<T: Display, E: Display> Display for Result<T, E>    # Ok(7) / Err(nope)
```

An impl belongs beside the type it is for, so only the first two live in
`std::fmt`: an array and a tuple are the two types with no module of their own.
`Option`'s and `Result`'s live in `std::option` and `std::result`, exactly as
`impl<T: Ord> Ord for Option<T>` already did — which also decides who sees
them, since a module only selects from impls it can reach.

The element goes through `Display` in turn, which is the point: a `[Point]`
renders with `Point`'s own `to_string`. It also means a `String` element renders
bare (`[a, b]`, not `["a", "b"]`) — `Display` answers "render this for a
reader", and quoting is a debugging affordance. Arity is per-impl: a tuple's
length is part of its type and nothing can be generic over it, so a 3-tuple
would need its own impl.

The bound is on the *impl's* type parameter, and it is checked where the impl
is selected — so `"{v}"` on a `[Widget]` fails at the interpolation, naming
`Widget`:

```
error: cannot interpolate a value of type '[Widget]': it does not implement 'Display'
note: an impl exists, but it requires 'Widget: Display'
```

The cost of shipping these is that a program cannot write its own: naming
`Display` requires importing `std::fmt`, which makes the std impl visible, and
coherence rejects the pair (`tests/fail/display_container_conflict.dt`). That
is also why none could be shipped before impls became module-granular — the
claim would have applied to every program in the language.

**`print` is deliberately not held to `Display`.** It renders any value
structurally, through the runtime's own walk: `print(p)` gives
`Point { x: 1, y: 2 }` with no impl at all. The two are different questions —
"show me what this is" is a debugging view the runtime can always answer, while
"render this for a reader" is the type's own decision, and only the second
needs a trait.

`float` is the one rendering interpolation cannot ask for: `"{f}"` gives the
shortest decimal that round-trips, which is the only rendering the VM has.
There is no format-spec grammar inside `{}`.

### `std::option`

```
pub enum Option<T> { Some(T), None }

impl<T> Option<T> {
    fun is_some(self) -> Bool
    fun is_none(self) -> Bool
    fun unwrap(self) -> T                # panics on None
    fun expect(self, message: String) -> T
    fun unwrap_or(self, fallback: T) -> T
    fun unwrap_or_else(self, fallback: fun() -> T) -> T
    fun map<U>(self, f: fun(T) -> U) -> Option<U>
    fun and_then<U>(self, f: fun(T) -> Option<U>) -> Option<U>
    fun filter(self, pred: fun(T) -> Bool) -> Option<T>
    fun otherwise(self, other: Option<T>) -> Option<T>
}

impl<T: Ord> Ord for Option<T>          # None sorts before every Some
impl<T: Display> Display for Option<T>  # Some(5) / None
```

An ordinary generic enum with an ordinary generic inherent impl — nothing
about it is known to the compiler, and `Option` is not in scope until it is
imported (`use std::option::Option;`). It is not a prelude.

The `or` combinator is spelled `otherwise`, because `or` is a keyword; by the
naming convention the rest of the module follows, `or_else` would be the
closure-taking form, which does not exist yet.

`unwrap`/`expect` arrived with `std::panic` — they are the accessors that
cannot be written without a way to fail, and every other one here exists to
name what to do instead.

`impl<T: Ord> Ord for Option<T>` is the first place one std module imports
another — it is an ordinary `use std::cmp::Ord;`, and the registry deduplicates
it against a program's own import of `std::cmp` (`tests/run/std_option.dt`).

### `std::panic`

```
@native("panic_abort")
pub fun panic(message: String) -> Never;
```

`panic` is the only std function that promises never to come back, and `Never`
is the type that says so. It is not a new type — it is the one `return` and
`break` already gave an expression — but until this module nothing could *name*
it in a signature.

Naming it is what makes the promise usable. `infer_unify` lets `Never` stand in
for any type, so `return panic(msg)` satisfies any return type at all:

```
fun unwrap(self) -> T {
    match self {
        Option::Some(v) => { return v; },
        Option::None => { return panic("called 'unwrap' on a 'None' value"); },
    }
}
```

That single rule is the whole of what `std::result` and `Option::unwrap` needed
(`tests/pass/never_type.dt`).

**There is no unwinding and no `catch`.** A panic sets the same error a failing
native sets, so the VM reports it at the call site, prints the frames beneath
it, and stops. Recovering would need a story for what a half-finished frame
leaves behind, and the language has none.

The message is an ordinary `String`, so a call site can build one by
interpolation. A panic *inside* a generic can name the value that caused it
only where the type parameter is bounded (`E: Display`); `Option::unwrap` and
`Result::unwrap` are not, so their messages are fixed and `expect` takes one
from the caller.

### `std::result`

```
pub enum Result<T, E> { Ok(T), Err(E) }

impl<T, E> Result<T, E> {
    fun is_ok(self) -> Bool
    fun is_err(self) -> Bool
    fun ok(self) -> Option<T>            # discard the error
    fun err(self) -> Option<E>
    fun unwrap(self) -> T                # panics on Err
    fun expect(self, message: String) -> T
    fun unwrap_or(self, fallback: T) -> T
    fun unwrap_or_else(self, fallback: fun(E) -> T) -> T
    fun map<U>(self, f: fun(T) -> U) -> Result<U, E>
    fun map_err<F>(self, f: fun(E) -> F) -> Result<T, F>
    fun and_then<U>(self, f: fun(T) -> Result<U, E>) -> Result<U, E>
}

impl<T: Display, E: Display> Display for Result<T, E>   # Ok(7) / Err(nope)
```

The type `?` is named after. The operator does **not** know this module — it
recognises the shape structurally (see "`?` propagation"), which is why a
program could always propagate through an enum of its own. What the module adds
is the one everybody would otherwise write, its combinators, and the two
accessors `std::panic` unlocked (`tests/run/std_result.dt`).

`unwrap_or_else` hands the closure the *error*, unlike `Option`'s, which takes
none: recovering from a failure usually wants to know what it was. `map_err` is
the mirror of `map` and is what lets a caller cross two error types, since `?`
itself requires the enclosing function to return the very same enum.

### Native functions

The standard library cannot be written entirely in ducktape: `print` has to
reach `value_print`, and no expression in the language does that. So a std
module may declare a function **bodyless**, with an attribute naming the C
implementation:

```
@native("io_print")     pub fun print<T>(value: T);
@intrinsic("array_len") pub fun len<T>(xs: [T]) -> Int;
```

The *signature* is ordinary ducktape and goes through the ordinary checker;
the attribute supplies the body. The two are exclusive in both directions — an
attributed function ends at the `;` and has no block, and a function without an
attribute must have one — so there is never a question of which body wins.

The name in the parentheses is a key into a registry C fills in, not a symbol
name: an unknown key is a compile error pointing at the attribute, listing what
is available.

Two tiers:

| | `@native` | `@intrinsic` |
|---|---|---|
| what it is | a C function | a bytecode opcode |
| how a call compiles | `OP_CALL` into C, no frame opened | the opcode, inline |
| usable as a value | yes — it takes an ordinary global slot | no: an opcode is not something a slot can address |
| in a bytecode image | written by name, re-bound at load | never appears; the opcode is already in the code |

Nothing else about a native is special. It obeys `pub`, is imported by name or
alias like anything else, and a *generic* native is never monomorphised — the
runtime is uniform in type arguments, so one C body serves every `T`, which is
why `print<T>` works at all.

Attributes are only accepted on a top-level `fun`. An impl method or a trait
method cannot be native.

Nothing restricts them to `std/` — a user module may declare a native too. The
registry is closed, so the only names available are the ones the binary already
provides, and the *decision* about what C exposes stays in C. Restricting the
attribute by module would add a rule without adding a guarantee.

### `std::io`, `std::array`, `std::string`

```
std::io      print<T>(value: T)                     # @native

std::array   len<T>(xs: [T]) -> Int                 # @intrinsic (OP_LEN)

std::string  len(s: String) -> Int                  # @native
             slice(s: String, from: Int, to: Int) -> String

std::fmt     float(value: Float, precision: Int) -> String   # @native
```

**`print` is not a builtin and is not in scope by default** — `use
std::io::print;` is a real import of a real module, and forgetting it is an
ordinary "cannot find 'print' in this scope" error. There is no prelude.

`std::string::slice` and `std::fmt::float` are the std functions that can fail
without a `Result`: a native reports a runtime error by setting `ctx->error`,
and the VM raises it at the call site, exactly as `std::panic::panic` does.

A `Float` prints — and interpolates — as the shortest decimal that reads back
as the same double, always carrying a `.` or an exponent so it is never
mistaken for an `Int`: `1.0`, `0.3333333333333333`, `300.0`, `1e+18`, `1e-07`,
`-0.0`, `inf`, `-inf`, `NaN`. Exponent form takes over below `1e-5` and at
`1e17`; every form printed is also a literal the scanner accepts.

## Not yet implemented

| Gap | Behavior today |
|---|---|
| extra (non-trait) methods in a trait impl | tolerated as inherent methods (Rust rejects them) |
| `dyn Trait` for a non-object-safe trait | rejected where `dyn` is written, naming the method and the reason |
| coercing the abstract `Self` of a default body to `dyn Trait` | not offered — `self` inside a default body cannot be handed over as a trait object |
| recovering from a panic (`catch`, unwinding) | a panic reports at the call site and stops; there is no `catch` |
| `Display` for a tuple of arity other than 2 | arity is part of a tuple's type and nothing is generic over it, so each needs its own impl; only `(A, B)` ships |
| writing your own `Display` for a container | rejected as a conflict with the std impl, which naming the trait makes visible |
| width, alignment, or padding in a format | only `std::fmt::float(value, precision)`; there is no format-spec grammar inside `{}` |
| casting a `dyn Trait` back to its concrete type | no downcast; the coercion is one-way |
| `dyn Trait` with type arguments (`dyn Into<Int>`) | generic traits are not parameterised in `dyn` position |
| `mod` declarations | no such keyword; `use` is what pulls a file in |
| glob imports (`use a::*`) | not parsed; name each item |
| module-qualified paths (`geometry::Point`) | import the item and name it directly; the diagnostic says so |
| `pub` on impls, methods, and fields | parsed and ignored — a public type's fields and methods are all visible |
| overlap rules finer than "same trait, matching self types" | there is no orphan rule and no specialization: an impl may be written for any type, and two overlapping ones are simply refused wherever both are visible |
| visibility below module granularity (`pub(crate)` &c.) | `pub` is the only modifier |
| two spellings of one file (symlinks, unusual paths) | dedup is lexical, so the file would load twice and collide |
| top-level `var` (globals) | parses, then a registration diagnostic: move it into a function |
| tuple-struct struct-patterns `Pair { a, b }` | write the constructor spelling `Pair(a, b)` — "matching tuple struct with struct pattern syntax is not allowed" |
| variable shadowing diagnostics | a `var` may silently shadow an earlier one in the same scope (top-level *item* names do collide — that is an error) |
| overlapping method names across impls of one type | bare paths pick the first registered impl (inherent impls do not conflict; only trait impls are checked for overlap) |
| capturing a `for` loop variable in a closure | runs, but the closure sees the loop variable's *final* value (one shared cell), not a per-iteration copy — `runtime.md` "Closures & upvalues" |
| infinitely deep generic instantiation | `fun grow<T>(v: T) { grow([v]) }` type-checks but names a new instantiation at every level; codegen stops at 32 and reports it (`runtime.md` "Monomorphisation") |
| more than 65536 functions, counting one per instantiation | each instantiation takes a global slot, so a heavily generic program can outgrow the two-byte operand space (`runtime.md` "Bytecode") |
| an `@intrinsic` named as a value (`var f = len::<Int>;`) | an intrinsic is an opcode, so there is no body for a global slot to address — "is an intrinsic and can only be called directly" (an `@native` *can* be a value) |
| a generic function named as a value (`var p = print;`) | its type arguments have nothing to solve them — "cannot infer type for 'T'"; call it, or use a non-generic one |
| `@native` on an impl or trait method | attributes are only accepted on a top-level `fun` |
| generic `main` | nothing calls the entry point, so no instance is ever made — "'main' must not be generic" |
