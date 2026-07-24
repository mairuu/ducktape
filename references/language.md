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

trait Into<T> {                      # a trait may take type parameters
    fun into(self) -> T;             # supplied wherever the trait is named
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
| `Char` | one Unicode scalar value — see "`std::char`" |
| `StringBuf` | a growable text buffer — see "`std::strbuf`" |
| `()` | unit |
| `Never` | code that does not come back — see "`std::panic`" |
| `(A, B)` | tuple |
| `[T]` | array of `T` |
| `fun(A, B) -> R` | function type |
| `Point<Int>` | generic instance |
| `dyn Drawable`, `dyn Into<Int>`, `dyn Iterator<Item = Int>` | trait object — see "Trait objects" |
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
  String) renders itself; anything else must implement `std::fmt::Display`. A
  `:` format spec (`"{v:>8}"`, `"{f:.3}"`) is sugar for the `std::string::pad_*`
  and `std::fmt::float` calls — see `std::fmt`.
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
  impls define the same name). A builtin type qualifies a path like any
  declared one (`Float::from(7)`, `Int::from('A')`), which is what makes an
  impl written for a primitive reachable by name.
- A path may also be qualified by a **type parameter**: `T::make(1)`, or
  `Self::make(1)` inside a trait default body. The parameter names no
  definition, so the item is looked up on its *bounds* — first bound declaring
  the name wins, as for a method call — and which impl supplies the body waits
  until `T` is concrete. This is how a trait's *associated functions* (a
  signature with no `self`, so there is no receiver to dispatch on) are
  called at all. A method may be called the same way with the receiver passed
  explicitly (`T::value(v)`), since a method is an associated function whose
  first parameter is `self` (`tests/run/assoc_bound_call.dt`).
- A call may also be qualified by a **fully applied trait reference**:
  `Into::<Fahrenheit>::into(c)`. When a type implements one generic trait
  several times (`Celsius` goes `Into<Fahrenheit>` *and* `Into<Kelvin>`), a bare
  `c.into()` cannot say which; naming the trait does. The receiver is passed
  positionally (`Trait::<Args>::method(recv, ..)`), and its type together with
  the reference selects the impl — so this reads the *receiver* to disambiguate
  where the qualified `Steps::from(v)` reads the *argument*. Only a method with a
  `self` parameter qualifies this way (an associated function has no receiver to
  choose by), the impl must *define* the method (a defaulted one is reached by
  the receiver call), and the receiver must have a known concrete type
  (`tests/run/trait_qualified_call.dt`).
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
- A bound may name an *earlier* type parameter of the same list
  (`fun conv<T, U: Into<T>>(v: U) -> T`), which is what a generic trait is for.
  The bound is opened alongside the parameter it mentions, so what gets
  checked once inference settles is `Into<Int>` and not the literal `Into<T>`
  (`tests/fail/trait_bound_arg_forwarded.dt`). Left to right: a bound cannot
  name a parameter declared after it.

### Generic traits

A trait may declare type parameters of its own:

```
trait Into<T> {
    fun into(self) -> T;
    fun into_pair(self) -> (T, T) { (self.into(), self.into()) }
}

impl Into<Fahrenheit> for Celsius { fun into(self) -> Fahrenheit { .. } }
impl Into<String>     for Celsius { fun into(self) -> String { .. } }
```

They behave as ordinary generic parameters of every signature the trait
declares — including its default bodies, which are compiled once per
`(Self, arguments)` pair like any other generic function. A trait *reference*
(`Into<Int>`) is what supplies them, and one is written at each of the three
places a trait can be named: an impl head, a bound, and a `dyn`. They are
never inferred there, so the argument count is checked at each
(`tests/fail/trait_type_args.dt`, `tests/fail/trait_missing_type_args.dt`) —
a bare `Into` is not a usable reference, since it has said nothing about what
it converts to.

The arguments are part of what a bound asks for: `impl Into<Int> for S` does
not answer `T: Into<String>` (`tests/fail/trait_type_arg_unsatisfied.dt`).
They are equally part of coherence, so one type may implement one trait
several times as long as the arguments differ — `Celsius` above is two impls,
not a conflict.

That makes a *bare* method call the one place the receiver alone cannot
decide: `c.into()` has two bodies to choose from. The expected type breaks the
tie (`var f: Fahrenheit = c.into()`), and a call through a bound never needs
it, since the bound names the reference. With neither, the first matching impl
wins — the same rule overlapping method names have always had.

See `tests/run/generic_trait.dt`.
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
outside the receiver (so no `-> Self`, no `other: Self`) — a trait object has
erased the concrete type, and those signatures all need it back. The rule is
checked where `dyn Trait` is *written*, not at the trait declaration, so a
trait may freely be static-dispatch-only and still be used as a bound
(`tests/run/trait_default.dt` leans on all three).

**A `Self.Item` is the exception, and it is named at the `dyn`:**

```
trait Iterator {
    type Item;
    fun next(self) -> Self.Item;
}

var it: dyn Iterator<Item = Int> = Counter { n: 41 };
print(it.next());                              # 42
```

`Self` itself cannot be recovered — that is what the coercion threw away —
but `Self.Item` is not the erased type, it is a *function of* it, and a
function of an erased thing can be pinned by writing down its result. So a
trait object is a trait plus one binding per associated type the trait
declares. Every one is **required**, whether or not a method mentions it:
`dyn Iterator` on its own is not an under-checked type, it is not a type
(`tests/fail/dyn_assoc_missing.dt`).

The binding is part of the type. `dyn Iterator<Item = Int>` and
`dyn Iterator<Item = String>` are two types; a default body reached through
each is a separate instantiation, and coercing to one needs an impl that
binds the same type — implementing the trait is no longer enough
(`tests/fail/dyn_assoc_mismatch.dt`). The binding may itself be a type
parameter (`[dyn Iterator<Item = T>]`), in which case the impl is what solves
it. See `tests/run/dyn_assoc.dt`.

The bracket list carries **two different things**: the trait's own type
arguments, which are positional, and its associated-type bindings, which are
named — `dyn Into<Int>`, `dyn Iterator<Item = Int>`, or
`dyn Pipe<String, Out = Int>` for a trait with both. Arguments come first
(`tests/fail/dyn_binding_before_arg.dt`). A trait argument may also be left to
the impl to solve (`[dyn Into<T>]`), which works exactly when one visible impl
answers: a type implementing the trait at two different arguments is a
question only the source can settle
(`tests/fail/trait_arg_ambiguous.dt`).

A trait's own type parameters cost it no object safety. They are written down
by whoever names the `dyn`, so unlike `Self` they were never erased.

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
use geometry::Point;                 # <root_dir>/geometry.dt — item Point
use geo::point::{Point, Line};       # <root_dir>/geo/point.dt, two items
use util::Maybe as Opt;              # imported under a different name
use std::string;                     # the *module* string, as a qualifier
```

For a **braced** import, the leading segments name the *file* and the brace
group names the *items*. For a **bare** import (no braces), if the whole path
names a module file it is a *module import* — nothing is imported by name;
instead the module is bound as a **qualifier** (see below). Otherwise the last
segment is a single item of the prefix module. Paths are relative to the
directory of the root file — the one search root — for every module, including
modules that are not the root. A file reached by two different importers is
loaded once.

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

### Module-qualified paths

A bare `use a::b;` whose path names a module *binds the module* rather than
importing one of its items. The name (or an `as` alias) becomes a qualifier, and
`b::thing` names a `pub` item of `b` — a function, a type, an associated call,
an enum variant — without pulling it into scope by name:

```
use std::string;
use std::array;

var n = string::len(s);   # two `len`s, told apart by their module
var m = array::len(xs);   # — no import could name both at once
```

The qualifier reaches exactly the `pub` items a `use a::b::thing;` would (private
items and un-re-exported imports stay hidden), so the two spellings never
disagree about what is visible. Binding a module is what makes the qualifier
work: importing an *item* (`use a::Thing;`) does **not** let you write
`a::Thing` — the diagnostic points you at `use a;`. A module is only ever a
qualifier: naming it where a value or type is expected is an error.

This is the same reachability model one level up. `use` already means "make
reachable" — it loads the file, brings in its impls, and adds a dependency edge.
A module import adds one thing on top: a name to spell what the `use` reached.
So a qualified path needs no new runtime and no new resolution machinery — the
first segment of a path may now be a module, and the rest resolves against that
module's scope exactly as a top-level path resolves against the current one.

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
live one module further away. It applies to std exactly as to anything else,
which is worth knowing before writing your own impl for a primitive: `use
std::array::push;` reaches `std::option` (that is what `pop` returns), and so
`std::fmt` and `std::cmp` beyond it, and `std::string` beyond *that* — so every
impl those modules ship is visible too.

That chain is why the direction of a std module's own imports is a design
decision rather than bookkeeping: what an import costs its dependents is the
*impls* the imported module ships, not its size. `std::cmp` can afford to
import `std::string` and `std::char` because both are modules of free functions
with no impls at all.

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
`std::convert`, `std::option`, `std::result` and `std::text` need none, `std::io`
and `std::panic` are nothing but, and `std::array`, `std::string`, `std::strbuf`
and `std::char` are the mixed case — a handful of natives, and every other
function written on top of them in ducktape. `std::fmt::Display` is the only std name the
*compiler* knows.

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

`Ord` is implemented for `Int`, `Float`, `Char` and `String`, which is the point worth
noticing: **a trait can be implemented for a primitive**, so the standard
library extends the built-in types with exactly the machinery user code uses.
Your own types join the same trait and `max`/`min` work on them unchanged
(`tests/run/std_cmp.dt`).

It is also implemented for the two containers with no module of their own,
`[T]` and `(A, B)`, beside the trait the way `std::fmt` ships their `Display`
(`tests/run/container_ord.dt`):

```
use std::cmp::{Ord, max};
print([1, 2, 3].cmp([1, 2, 4]));  # -1  lexicographic, first difference decides
print([1, 2].cmp([1, 2, 3]));     # -1  a prefix sorts before what extends it
print((2, 1).cmp((1, 9)));        #  1  a tuple field by field, .0 before .1
print(max([1, 3], [1, 2]));       # [1, 3]
```

Both need `T: Ord` (each `A`/`B` for the tuple), and the element is ordered
through *its* own `Ord` — so `[[T]]`, an array of tuples of strings, all sort
with no order written per program. A tuple's arity is part of its type and
nothing is generic over it, so `(A, B)` is the only tuple width shipped, the
same limit `Display` has. The array impl needs an array's length while
`use std::array` would close a dependency cycle (`std::array` reaches
`std::option`, which reaches `std::cmp`); `len` is an `@intrinsic`, though —
a *spelling* for `OP_LEN` rather than a definition a module owns — so `std::cmp`
names the opcode a second time and the cycle that blocks the impl's type does
not block the length it needs.

**`String` is ordered by a trait, not by an operator.** `<` and `>` stay
numeric — comparing two Strings is `a.lt(b)`, or `max`/`min`/`clamp`
(`tests/run/string_ord.dt`):

```
use std::cmp::{Ord, max};
print(max("apple", "pear"));      # pear
print("Zebra".gt("apple"));       # false — byte order, not locale
```

The comparison is by *bytes*, which for well-formed UTF-8 is code-point order:
a multi-byte sequence's lead byte is above every ASCII byte. A prefix sorts
before what extends it, and there is no locale, case-folding or normalisation.

The interesting part is what interning does *not* buy here. `==` on a String is
a pointer compare because two equal strings are one object in the intern table
— but pointer *order* is allocation order, so ordering gets nothing from the
table beyond the equal case, and has to walk the bytes. That walk is
`std::string::compare`, and it has to be a native for the same reason `push` is:
the finest handle ducktape has on a String's contents is `slice`, and comparing
two one-byte slices would need the ordering being defined.

`Char` is ordered the same way, and by the same rule about where the impl
goes: `std::cmp` imports `std::char` for it. That one needs no native at all —
`std::char::code` hands the comparison two Ints, and `<` on an Int is an
opcode — which is the difference between ordering a character and ordering a
string of them.

The impl lives in `std::cmp`, beside the trait, so `std::cmp` imports
`std::string` (and `std::char`) rather than the other way round. That direction is deliberate:
impl visibility is transitive through `use`, and `std::string` ships no impls,
so importing it adds nothing to what a `use std::cmp::…` already delivers.
Putting the impl in `std::string` would have handed a program that only wanted
`len` every `Ord` impl — and with them coherence's refusal to let it write
its own `impl Ord for Int`. **An import's cost is measured in impls, not in
code.** `impl Display for String` living in `std::fmt` sets the same precedent.

`Ord` is deliberately *not* object-safe — `other: Self` means a caller must
know the concrete type — so it is a bound, never a `dyn Ord`. That is the rule
working as designed: object safety is only demanded where `dyn` is written.

### `std::convert`

```
pub trait From<T> { fun from(value: T) -> Self; }
pub trait Into<T> { fun into(self) -> T; }

impl<T, U: From<T>> Into<U> for T { .. }        # the blanket

impl From<Int> for Float                        # widening
impl From<Char> for Int                         # the scalar value
```

Two traits and one relation: a type that can be made *from* an `Int` is an
`Int` that goes *into* it. **A program writes `From` impls and never writes
`Into`** — the blanket supplies the other direction for every one of them.

```
use std::convert::{From, Into};

impl From<Celsius> for Fahrenheit { fun from(c: Celsius) -> Fahrenheit { .. } }

var f = Fahrenheit::from(c);            # the direction that names its answer
var g: Fahrenheit = c.into();           # the same conversion, annotated
```

The two spellings are not interchangeable, and the difference is the whole
reason the pair is written from two ends. `from` is qualified by the type it
produces, so nothing else has to say what the conversion is. `into` is a
method whose *receiver cannot decide the answer* — `7.into()` could produce
anything — so the impl is pinned by the type the result flows into: an
annotation, a parameter, a return type. With none of those it is an error
("no method named 'into' found for type 'Int'",
`tests/fail/into_without_expected_type.dt`); a bound that names the reference
(`fun scaled<T: Into<Float>>`) needs no hint at all, since the reference is
written down.

When a type is the target of **more than one `From`**, the qualified call reads
its argument to choose (milestone 30): with `impl From<Int> for Steps` and
`impl From<Char> for Steps` both in scope, `Steps::from(26)` and
`Steps::from('a')` reach different impls, and an argument that fits none names
what it could not satisfy and lists what each candidate takes
(`tests/run/assoc_select.dt`, `tests/fail/assoc_select_no_impl.dt`). The
argument has to type on its own to do the choosing, so a value that would itself
need the impl chosen first — `Steps::from(None)` — cannot be disambiguated.

The receiver-side mirror is a **trait-qualified call** (milestone 31): where the
expected type is unavailable or a type goes `Into` several ways, name the trait
and the receiver settles the rest. `Into::<Fahrenheit>::into(c)` and
`Into::<Kelvin>::into(c)` reach different impls of a `Celsius` that has both, and
neither needs an annotation — the reference is written down. It reads the
receiver to choose where `Steps::from` reads the argument, the two ends of one
selection (`tests/run/trait_qualified_call.dt`). See "Paths" above for the rules
that bound it.

Importing the module **costs a program its own `Into` impls**: coherence is
blind to an impl's bounds, so the blanket overlaps every `impl Into<X> for Y`
that could be written (`tests/fail/into_impl_conflicts_with_blanket.dt`).
The reflexive `impl<T> From<T> for T` is deliberately absent for the same
reason one step worse — it would overlap every `From` impl, leaving a trait
nobody could implement.

Only two conversions ship, and the restraint is the point: an import's cost is
measured in impls (see `std::cmp`). Both are lossless. There is no
`From<T> for String` — rendering is `Display`'s question and `to_string`
already answers it — and no `From<Float> for Int`, since `as Int` truncates
and which way it should round is a question a fallible conversion would have
to ask. `From` is not object-safe (it produces a `Self`), while `Into` is; see
`tests/run/convert.dt`.

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

`float` is the one rendering interpolation cannot ask for on its own: `"{f}"`
gives the shortest decimal that round-trips, which is the only rendering the VM
has. `float` renders a `Float` to a fixed precision, and `std::string`'s
`pad_start`/`pad_end`/`pad_center` lay a rendered string out to a width. Width
there is a *character* count, not bytes, since alignment is a display question;
the value has to be rendered to a `String` first, which is exactly why these are
String→String operations.

A format spec is the terse spelling of those calls (milestone 35):

```
"{v:>8}"       # pad_start(v, 8, ' ')   — right-aligned to width 8
"{v:<8}"       # pad_end(v, 8, ' ')     — left-aligned
"{v:^8}"       # pad_center(v, 8, ' ')  — centred
"{v:'-'^8}"    # a fill char, written as a char literal
"{f:.3}"       # float(f, 3)            — three decimal places
"{f:>10.3}"    # pad_start(float(f, 3), 10, ' ')  — both, fused
```

The spec is `:` then an alignment (`<` `>` `^`) with a width, an optional
leading fill char, and an optional `.N` precision — or a `.N` alone. It is
sugar and nothing more: the checker rewrites the segment into the same `pad_*`
and `float` calls you could write by hand, so codegen, the VM and the image
format never see a spec. The value is rendered to a `String` first (a primitive
via the VM, a `Display` type via `to_string`), which is why a width applies to
anything renderable while a precision applies only to a `Float`. Because the
compiler generates the `pad_*`/`float` calls, it must resolve those names
itself — so they are lang items like `Display`, and a spec needs its module
(`std::string` for a width, `std::fmt` for a precision) present in the program,
reported if it is not. What has no spelling is a *dynamic* width (`{v:>{n}}`):
the width and precision are literals.

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

### `std::io`, `std::array`, `std::string`, `std::strbuf`

```
std::io      print<T>(value: T)                     # @native

std::array   len<T>(xs: [T]) -> Int                 # @intrinsic (OP_LEN)
             push<T>(xs: [T], value: T)             # @native
             pop<T>(xs: [T]) -> Option<T>
             first<T>(xs: [T]) -> Option<T>
             last<T>(xs: [T]) -> Option<T>
             is_empty<T>(xs: [T]) -> Bool
             clear<T>(xs: [T])

std::string  len(s: String) -> Int                  # @native, in bytes
             slice(s: String, from: Int, to: Int) -> String   # @native
             compare(a: String, b: String) -> Int   # @native
             chars(s: String) -> [Char]             # @native
             join(parts: [String], sep: String) -> String
             concat(parts: [String]) -> String
             repeat(s: String, n: Int) -> String
             from_chars(cs: [Char]) -> String
             pad_start(s: String, width: Int, fill: Char) -> String
             pad_end(s: String, width: Int, fill: Char) -> String
             pad_center(s: String, width: Int, fill: Char) -> String

std::strbuf  new() -> StringBuf                     # @native
             push(b: StringBuf, s: String)          # @native
             push_char(b: StringBuf, c: Char)       # @native
             push_int(b: StringBuf, n: Int)         # @native
             len(b: StringBuf) -> Int               # @native
             clear(b: StringBuf)                    # @native
             build(b: StringBuf) -> String          # @native
             is_empty(b: StringBuf) -> Bool

std::char    code(c: Char) -> Int                   # @native
             from_code(n: Int) -> Char              # @native
             is_ascii/is_digit/is_lower/is_upper(c: Char) -> Bool
             is_alpha/is_alnum/is_whitespace(c: Char) -> Bool
             to_upper(c: Char) -> Char              # ASCII only
             to_lower(c: Char) -> Char              # ASCII only
             to_digit(c: Char) -> Int

std::fmt     float(value: Float, precision: Int) -> String   # @native
```

**`print` is not a builtin and is not in scope by default** — `use
std::io::print;` is a real import of a real module, and forgetting it is an
ordinary "cannot find 'print' in this scope" error. There is no prelude.

`std::string::slice` and `std::fmt::float` are the std functions that can fail
without a `Result`: a native reports a runtime error by setting `ctx->error`,
and the VM raises it at the call site, exactly as `std::panic::panic` does.

**An array grows.** `push` appends, reallocating the buffer behind the array
when it is full; an array literal starts exact-fit, so `[1, 2, 3]` and `[]` are
both ordinary starting points:

```
var xs: [Int] = [];
push(xs, 1);
push(xs, 2);
print(pop(xs).unwrap());   # 2
```

Two consequences follow from what an array already was, rather than from
growth itself:

- **An array is a reference.** Assignment binds a second name to the same
  object, so a push through either is seen through both — which has been true
  of `xs[0] = v` since arrays existed, and there is no way to spell "by
  reference" because there is nothing else to spell.
- **`for x in xs` re-reads the length each iteration**, so pushing to the array
  being iterated extends the loop rather than iterating a snapshot. Nothing
  prevents it; there is no borrow checker.

Only `push` is written in C, alongside a private raw remove-last. They are the
two things an array cannot express about itself — a slot that did not exist
before, and one fewer than there was — and everything else in the module is
ducktape on top. That is what lets `pop` answer with an `Option`: a native's
contract is "n values in, one out" and it has no handle on the `VariantDef` an
enum instance needs, so a native *cannot* build an `Option` at all. Popping
does not release capacity; the buffer is returned when the array is collected.

**A `String` is built with a `StringBuf`, which lives in `std::strbuf`.** `a +
b` allocates a new String and interns it, so growing one a piece at a time
re-interns the whole accumulation at every step. A buffer appends in place
instead, and `build` interns once:

```
use std::strbuf;

var b = strbuf::new();
for word in words {
    strbuf::push(b, word);
    strbuf::push(b, " ");
}
print(strbuf::build(b));
```

The buffer is its own module so its operations can take the plain names `len`,
`push` and `is_empty` — the ones `std::string` spends on `String` and
`std::array` on `[T]`. Before module-qualified paths they had to be dodged
(`buf_len`, `push_str`); now `strbuf::len(b)` sits beside `string::len(s)` with
nothing aliased apart.

`StringBuf` is a *separate type* from `String`, not a mutable flavour of one,
and the reason is interning: a String is filed in the runtime's table under the
hash of its bytes, and two equal strings are the same pointer — which is what
makes `==` on strings a pointer compare. Bytes that change cannot be in that
table. So a buffer is the object deliberately kept out of it, and `build` is a
one-way door: it copies, non-destructively, so a buffer may be built from more
than once and appended to in between.

Two smaller consequences, both visible from a program:

- **A buffer is a reference**, like a `[T]`: a second name is the same object,
  so a `push` through either is seen through both.
- **A buffer has no `Display` impl**, so `"{b}"` is an error naming the type.
  Rendering one is `build`, which allocates and interns, and that is a decision
  worth making out loud. `print(b)` still shows it, as `StringBuf("…")` — the
  debug view says which of the two kinds it is looking at.

Seven of `std::strbuf`'s functions are written in C — existing, growing (from a
String, a Char or an Int's digits), emptying, its length, and becoming a String
— and `is_empty` is ducktape on `len`. `push_int` puts a number's digits in
without interning a `"{n}"` String to carry them, and `clear` drops the length to
zero while keeping the capacity, so one buffer can be reused across a loop. `std::string`'s `join`, `concat`, `repeat` and `from_chars`
are the String-shaped conveniences on top, built through the buffer, the same
split `std::array` makes; `repeat` is the one that shows what the buffer buys —
it copies bytes straight in, allocating no String per copy. `std::string` now
reaches only `std::strbuf`, which imports nothing and ships no impls, so
importing `std::string` still hands a program no impls it did not ask for — the
property that makes it safe for `std::cmp` to depend on it for `impl Ord for
String`. What `std::string` must *not* reach is `Option` or `std::array`, which
would close the cycle `string → option → cmp → string`; the buffer is a pure
leaf below it, so the edge to it is free.

A `Float` prints — and interpolates — as the shortest decimal that reads back
as the same double, always carrying a `.` or an exponent so it is never
mistaken for an `Int`: `1.0`, `0.3333333333333333`, `300.0`, `1e+18`, `1e-07`,
`-0.0`, `inf`, `-inf`, `NaN`. Exponent form takes over below `1e-5` and at
`1e17`; every form printed is also a literal the scanner accepts.

### `std::char`, and what a `String` is made of

A `Char` is **one Unicode scalar value** — a code point that is not a surrogate
half — written between single quotes:

```
var a = 'x';
var accented = 'é';        # a multi-byte character, written directly
var newline = '\n';
var snowman = '\u{2603}';  # for one that cannot be typed
```

The escape set is the string one with `\{` traded for `\'`: a character literal
has no interpolation, and a quote is what closes it. `\u{…}` takes one to six
hex digits. A literal holding no character (`''`), more than one (`'ab'`), or a
value that is not a scalar value (`'\u{D800}'`) is a scanner error.

A `Char` is a **primitive**, so it behaves like an `Int` throughout: it is a
value rather than a heap object, `==` compares scalar values, `"{c}"` renders
it with no `Display` impl involved, and `match c { 'q' => …, _ => … }` works
the way an `Int` match does — a wildcard is required, since the domain is far
too large to enumerate.

**A `String` is bytes and a `Char` is a character, and the language keeps the
two apart.** `std::string::len` counts *bytes*, `slice` cuts at *byte* offsets,
and `compare` walks bytes; `std::string::chars` is the only crossing:

```
var s = "héllo";
len(s);              # 6 — bytes
alen(chars(s));      # 5 — characters
from_chars(chars(s)) == s;   # true
```

There is deliberately **no `char_at(s, i)`**. The index would be a byte offset,
a byte offset is not a character position, and offering that spelling would
make confusing the two the default rather than the mistake. The crossing is a
conversion in both directions, and `from_chars` goes back through a
`StringBuf`, which `push_char` is what makes possible.

`chars` is a **runtime error if the string is not valid UTF-8**, which a
program can provoke: `slice` cuts at byte offsets, so it can halve a multi-byte
sequence. A String is a byte string; only a `Char` promises to be a character.

Everything else in `std::char` is ordinary ducktape over `code`/`from_code` —
the one thing a Char cannot say about itself is its number, and once it can,
every classification is a range test and every case conversion is an addition.
**The classifications are ASCII-only**: `is_alpha('é')` is false and
`to_upper('é')` is `'é'` unchanged. Full Unicode case mapping is a table, not a
range test, and shipping a range test under that name would be right for
English and quietly wrong elsewhere.

`impl Ord for Char` (in `std::cmp`, beside the trait) is code-point order, so
`'Z'` sorts before `'a'` — the same order `impl Ord for String` gives, since
UTF-8 byte order and code-point order agree. As with `String`, ordering is the
trait and not the operator: `'a' < 'b'` is still "comparison requires numeric
types".

### `std::text`

`std::text` is searching, splitting, trimming and parsing, written entirely in
ducktape on the primitives `std::string` and `std::char` already offer:

```
use std::text::{starts_with, ends_with, find, contains, split, trim, parse_int};

starts_with("hello", "he");    # true
ends_with("hello", "lo");      # true
find("hello", "l");            # Some(2) — a byte offset
contains("hello", "ell");      # true
split("a,b,,c", ",");          # ["a", "b", "", "c"]
trim("  hi there  ");          # "hi there"
parse_int("-42");              # Some(-42);  parse_int("12x") is None
```

**It is a module of its own, not more of `std::string`, and the reason is a
dependency cycle rather than taste.** `std::cmp` imports `std::string` for
`compare` (the byte comparison `impl Ord for String` is written on); `std::option`
imports `std::cmp`; and every function in `std::text` either answers with an
`Option` or builds a `[String]`. So putting them in `std::string` would make it
import `Option` or array `push`, closing the loop `string → option → cmp →
string` — which the dependency graph rejects outright ("module cycle"). The
module everything else builds on cannot reach back up to them (it reaches only
`std::strbuf`, a pure leaf below it); the operations that do live one module
higher. This is `std::array` losing its leaf status once `pop` returned an
`Option`, taken to the point where the split is *forced*. `std::text` is also
where a module qualifier earns its keep: it wants both `string::len` (bytes) and
`array::len` (elements), which no `use` could import under one name.

Within the module the two views of text stay apart, the milestone-26 way:

- **A *position* is a byte offset**, because a position is one you will `slice`
  at. `find` and `split` are `slice`+`==` and never inspect a byte on its own —
  so `find("héllo", "llo")` is `Some(3)`, and that 3 feeds straight back into
  `slice`. The cost is that `find` slices and interns a candidate substring at
  every position (O(*n·m*) allocations), the honest price of a String whose only
  reader is `slice`: there is no `byte_at`, so a window has to be cut out to be
  compared.
- **A *character* is classified by its value**, so `trim` and `parse_int` cross
  to the `chars` view — whether something is a space or a digit is a fact about a
  character, not a byte, and an ASCII space is a single byte only by luck of the
  encoding.

`starts_with`/`ends_with` need neither a position nor a classification, so they
are the pure `slice`+`==` ones. The empty pattern is read consistently
everywhere: `starts_with(s, "")` and `find(s, "")` both succeed at 0, and
`split(s, "")` returns `s` whole in a one-element array (there is no position at
which `""` is *not*, so the loop that finds one would never end). `split` yields
one more piece than there were separators, so a leading, trailing, or doubled
separator each produces an empty piece. `parse_int` accepts an optional leading
`+`/`-` then one or more digits and nothing else — an empty string, a bare sign,
or a stray character is `None`, not a partial parse — and does **not** detect
overflow: a value past what an `Int` holds wraps.

## Not yet implemented

| Gap | Behavior today |
|---|---|
| extra (non-trait) methods in a trait impl | tolerated as inherent methods (Rust rejects them) |
| `dyn Trait` for a non-object-safe trait | rejected where `dyn` is written, naming the method and the reason |
| coercing the abstract `Self` of a default body to `dyn Trait` | not offered — `self` inside a default body cannot be handed over as a trait object |
| recovering from a panic (`catch`, unwinding) | a panic reports at the call site and stops; there is no `catch` |
| `Display` for a tuple of arity other than 2 | arity is part of a tuple's type and nothing is generic over it, so each needs its own impl; only `(A, B)` ships |
| writing your own `Display` for a container | rejected as a conflict with the std impl, which naming the trait makes visible |
| Unicode case mapping, folding, or normalisation | `std::char`'s classifications and `to_upper`/`to_lower` are ASCII-only; `String` comparison is raw bytes |
| indexing a `String` by character (`s[i]`) | there is none: `chars(s)` converts, because a byte offset is not a character position |
| a `String` that is guaranteed valid UTF-8 | it is a byte string — `slice` cuts at byte offsets, so `chars` reports a runtime error on a halved sequence |
| a *dynamic* width or precision in a format spec (`{v:>{n}}`) | the width and precision in a `{v:>8}` / `{f:.3}` spec are literals; a runtime value there has no spelling. The spec itself is sugar for `std::string::pad_*` / `std::fmt::float` (milestone 35) |
| casting a `dyn Trait` back to its concrete type | no downcast; the coercion is one-way |
| a trait's type arguments at a *bare* method call | the expected type breaks the tie between two impls of one generic trait, and pins an impl parameter the receiver cannot reach (`impl<T, U: From<T>> Into<U> for T`); with no expected type the first impl wins — or, where the parameter was only pinnable that way, no impl applies at all. The trait-qualified spelling (`Into::<Fahrenheit>::into(c)`) settles it explicitly without an expected type |
| disambiguating a qualified selection whose *argument is itself unresolved* (`Steps::from(None)`) | the argument (for `from`) or the receiver (for a trait-qualified `into`) must type on its own to choose the impl, so a value that would need the impl chosen first cannot be disambiguated |
| a bound naming a *later* type parameter (`fun f<U: Into<T>, T>`) | "unknown type: T" — bounds resolve left to right |
| `mod` declarations | no such keyword; `use` is what pulls a file in |
| glob imports (`use a::*`) | not parsed; name each item, or bind the module (`use a;`) and qualify |
| re-exporting a module qualifier (`pub use a;`) | a qualifier is not an item; `pub use` re-exports named items only |
| `pub` on methods and struct fields | rejected: `pub fun` in an impl is "expected impl item", `pub x` on a field is "expected field name". `pub` is only ignored on the `impl` keyword itself; visibility exists only on top-level items, at module granularity |
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
