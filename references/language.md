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

pub struct Point<T> { pub x: T, pub y: T, }  # pub struct; pub fields exposed too
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
| `Range` | what `a..b` / `a..=b` evaluate to — see "`std::iter`" |
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
p.x = 3;  p.x += 1;           # ... to a field, tuple element, or array slot too
xs[i] = v;  t.0 *= 2;         # a struct is a shared reference, so this mutates
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
  `+` also concatenates `String`s. Two numeric operands stay a built-in opcode
  (no import, no frame); a non-numeric operand desugars to `std::ops`, so
  `a + b` becomes `a.add(b)` — see `std::ops` below. On an unbounded generic
  the diagnostic asks for the bound (`add the bound 'T: Add'`) rather than
  refusing arithmetic outright.
- Comparison `< <= > >=`: numeric operands stay a built-in opcode (no import,
  no frame); a non-numeric operand desugars to `std::cmp::Ord`, so `a < b`
  becomes `a.cmp(b) < 0`, dispatched by the ordinary method machinery. A `Point`
  with `impl Ord for Point`, a bounded `T: Ord`, a `Char`, a `String` — all
  compare with the operator. Without `std::cmp` in the program the operator has
  no trait to name (the diagnostic says so), and on an unbounded generic it asks
  for the `T: Ord` bound. Only `cmp` is named by the rewrite, so `lt`/`le`/`gt`/
  `ge` are not lang items; `Ord` is one (keyed on `std::cmp`, like `Display`).
- `== !=` (structural: any two values of the same static type, including a
  generic `T` — the runtime compares them the way it compares two structs, so
  `a == b` on a generic yields `Bool`). Unlike ordering, equality is *never* a
  trait: it is a free, import-less, universal primitive, and routing it through
  an `Eq` would make the commonest operation depend on an import.
- Logic: keywords `and`, `or` (short-circuit), `not`. There is no `&&`/`||`.
- **There are no bitwise operators at all** — no `&`, `|`, `^`, `<<` or `>>`.
  Two of those spellings are already taken (`|` opens a closure, `^` is the
  centre-align token in a format spec) and the rest were never added. The
  consequence is larger than it looks: any operation defined in terms of bits
  is not merely awkward but *unwritable* in ducktape, which is why hashing
  reaches C for its mixing (`std::hash`) and why a table indexes with `%`
  rather than a mask (`std::map`). Division by a power of two is the available
  stand-in for a right shift.
- `Int` arithmetic **wraps** silently on overflow, two's complement
  (`4611686018427387904 * 4` is `0`). There is no checked or saturating form.
- `%` keeps the **sign of its left operand**: `(0 - 17) % 5` is `-2`, not `3`.
  Anything using `%` to pick an array index has to fold the sign itself.
- Unary minus `-x`: numeric, or a type implementing `std::ops::Neg` — the same
  rewrite with the operand as receiver and no argument (`-v` is `v.neg()`).
- `if cond { .. } else { .. }` is an expression; without `else` it is `()`.
- `while cond { .. }` and `for x in iter { .. }` evaluate to `()`; `iter` may be
  an array, a range, or any type that implements `Iterator` (see below).
- Ranges: `a..b`, `a..=b` — Int-only, first-class values (`var r = 0..10;`) of
  type `Range`, which is nameable (`fun span(r: Range)`) and carries one
  method, `r.iter()`.
- Casts: `x as T` — only `Int`↔`Float` and identity casts.
- String interpolation: `"x = {x}"` — a primitive segment (Int/Float/Bool/
  String) renders itself; anything else must implement `std::fmt::Display`. A
  `:` format spec (`"{v:>8}"`, `"{f:.3}"`) is sugar for the `std::string::pad_*`
  and `std::fmt::float` calls — see `std::fmt`.
- Calls: `f(a, b)`; functions are first-class (`var g = f; g(1)`).
- Closures use pipes: `|x, y| x + y`, `|n: Int| -> Int { return n * 2; }`.
  Unannotated params infer from a function-typed hint
  (`var f: fun(Int) -> Int = |x| x + 1;`) or from use; a closure whose
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
- **A bound may constrain an associated type**, written only in a `where`:
  `fun largest<I: Iterator>(it: I) -> Option<I.Item> where I.Item: Ord`. A
  plain bound constrains the *parameter*; this constrains what an impl binds
  the parameter's associated type to, which is the difference between naming
  `I.Item` in a signature and being able to do anything with a value of that
  type. Without it a generic over an iterator can move elements around but not
  compare or print them — which is why `fold` takes its operation as a closure.

  The name must be an associated type some bound on the parameter declares
  (`I.Nope` is an error, as is a deeper path like `I.Item.Inner`), and repeated
  predicates merge exactly as plain ones do, so `where I.Item: Ord, I.Item:
  Display` and `where I.Item: Ord + Display` are the same constraint. Inside
  the body the projection then satisfies those traits — `v > b` on two
  `I.Item`s resolves, and `I.Item` can be passed to another generic wanting
  `Ord`. The promise is discharged at the *instantiation*: `largest(counter)`
  is where `I` becomes `Counter`, so `Counter.Item` becomes `Int` and the
  question becomes an ordinary one about a real type ("type 'Widgets' does not
  satisfy 'I.Item: Ord': its 'Item' is 'Widget', which does not implement
  'Ord'"). Handing a bounded projection to something wanting the same bound
  requires restating it: inside a generic there is no impl to consult, so
  `I.Item` satisfies exactly what *this* declaration promised, and dropping the
  `where` is an error naming the one to add
  (`tests/fail/assoc_bound_relay.dt`).
- **A trait method's signature may carry a `where` too**, and it is the one
  place `Self` may be bound: `fun max(self) -> Option<Self.Item> where
  Self.Item: Ord`. This is what lets a *provided* method use an element rather
  than only pass it along, so a bounded reduce can be a combinator
  (`it.max()`, `it.min()`) instead of a free function. The clause may also
  constrain the method's own type parameters (`where T: Ord`), which is the
  ordinary kind.

  A bound on a parameter is discharged where that parameter is bound — one
  place, the instantiation. `Self` has no such place: it is decided by
  whichever receiver a call has, so the promise is discharged at *each call*,
  against that receiver. A concrete one reads its impl's binding and asks the
  ordinary question; an abstract one (a bounded `I`, or `Self` inside another
  default body) answers from its own declaration and so must restate the
  bound. A method carrying such a clause is left out of a `dyn` vtable —
  whether the bound holds is a question about the concrete type and the impls
  visible where the value was coerced, which is exactly what the coercion
  erased — so it joins `map`/`filter` in the per-method object-safety
  partition. It is still callable on a trait object, since the body is
  compiled at `Self = dyn Trait` instead of dispatched, and the clause is
  discharged there against the binding the object states. A bound on `Self`
  *itself* (`where Self: Ord`) is rejected *here*: that is a supertrait, and a
  supertrait is a property of the whole trait rather than of one method, so it
  has one spelling — `trait A: B`, below.

  A `where` may otherwise appear on a `fun` — free or in an impl block — and
  on an `impl` itself, where every method inside may rely on it.
- **A bound may fix an associated type outright**, written in the trait
  reference the same way a trait object writes it:
  `fun chain<J: Iterator<Item = I.Item>>(..)`. Where every other predicate says
  which *traits* a type satisfies, this one says which type a projection **is**
  — an equality rather than a constraint — and that difference is what it is
  for: `chain` yields one sequence then another, so the second source's element
  type has to *be* the first's, which no trait can express.

  Because it is an equality, it is not a fact checked at each use but a
  rewrite: `J.Item` is not a type the program can build at all — writing it
  yields `I.Item` — so the two are the same type rather than convertible ones,
  and nothing downstream has two spellings to reconcile. Two consequences
  follow. There is exactly one way to name that element, so a further
  constraint goes on it (`where I.Item: Ord`) and writing `where J.Item: Ord`
  is an error naming the spelling to use. And the promise has to hold: it is
  discharged where the parameter is bound, both at a call ("type 'Words' does
  not satisfy 'J.Item = Int': its 'Item' is 'String', not 'Int'") and at impl
  selection, so an adapter built from mismatched sources finds no impl rather
  than compiling one element type as another.

  The type it names need not be another projection: `I: Iterator<Item = Int>`
  reads as "an iterator of Ints", and a body may then use its elements as the
  Ints they are without bounding them further.

  The binding names an associated type the trait declares, may not be repeated
  with two different types, and may only be written where there is a parameter
  to constrain — not on a `where I.Item: ..` predicate, whose left side is
  already a projection.
- A bound may name an *earlier* type parameter of the same list
  (`fun conv<T, U: Into<T>>(v: U) -> T`), which is what a generic trait is for.
  The bound is opened alongside the parameter it mentions, so what gets
  checked once inference settles is `Into<Int>` and not the literal `Into<T>`
  (`tests/fail/trait_bound_arg_forwarded.dt`). Left to right: a bound cannot
  name a parameter declared after it.

### Supertraits

A trait may require another:

```
trait DoubleEnded: Iterator {
    fun next_back(self) -> Option<Self.Item>;
}
```

The list after `:` is spelled exactly like a type parameter's bound, because
that is what it is — a bound on `Self`, which has no `<..>` list to sit in.
Several are joined with `+` (`trait Described: Shape + Named`), and they
compose transitively (`trait Tagged: Described` reaches `Shape` too).

It says one thing, which has two readings, and both hold:

- **As an obligation.** `impl DoubleEnded for Span` is legal only where
  `impl Iterator for Span` is visible; otherwise the impl is rejected naming
  the missing one (`tests/fail/supertrait_missing_impl.dt`).
- **As an assumption.** A `T: DoubleEnded` bound offers everything `Iterator`
  declares — its methods (`it.next()`, `it.collect()`), its associated types
  (`T.Item`), and its own supers in turn — and `T` may be handed to anything
  asking for `T: Iterator`. The obligation is what makes this sound: the
  assumption is answered off the declaration, without searching for an impl.

The point of the feature is that `Self.Item` above means `Iterator`'s `Item`.
Written as an independent trait, `DoubleEnded` would have to declare an `Item`
of its own and nothing would make the two agree — a type could bind them to
different types and both impls would check.

Inside the trait, `Self` is bounded by the trait being declared, so a default
body reaches the supers the same way (`self.next()` inside a `DoubleEnded`
default is an ordinary call through a bound).

A trait may not be its own supertrait, directly or through a chain
(`tests/fail/supertrait_cycle.dt`).

`dyn Sub` works, and carries the supers: its vtable is laid out over the
closure, so `d.next()` on a `dyn DoubleEnded<Item = Int>` dispatches through
the same table `d.next_back()` does, and the `<Item = ..>` binding names a
supertrait's associated type through the sub. Object safety is decided over the
closure too — a *required* method a vtable cannot carry is fatal wherever it
was declared. And a `dyn Sub` satisfies a `Super` bound, so it may be handed to
a `<I: Iterator>` function or driven by a `for` loop
(`tests/run/dyn_supertrait.dt`).

See `tests/run/supertrait.dt`, `tests/run/supertrait_generic.dt`.

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

Not every trait can be one. A method is **dispatchable** — reachable through
the vtable — only if it takes `self`, has no type parameters of its own, does
not mention `Self` outside the receiver (so no `-> Self`, no `other: Self`),
and carries no bound on an associated type (`where Self.Item: Ord`) — a trait
object has erased the concrete type, and every one of those needs it back: the
last because whether the bound holds is a question about that type and the
impls visible where it was coerced. The rule is checked where `dyn Trait` is
*written*, not at the trait declaration, so a trait may freely be
static-dispatch-only and still be used as a bound
(`tests/run/trait_default.dt` leans on the first three).

Object safety is decided **one method at a time**. A method that isn't
dispatchable is fatal only if it is **required**: there would be no body to
reach and no default to fall back on. A **provided** method (one with a default
body) that isn't dispatchable is simply *excluded from the vtable* — the trait
stays object-safe. This is what lets a trait carry generic convenience methods
and remain a `dyn`: `Iterator`'s `map`/`filter` are excluded (a type parameter,
a `Self`-shaped return) and so are `max`/`min` (a bound on `Self.Item`), yet
`dyn Iterator` is a type, and `collect` — dispatchable, since `[Self.Item]` is a
projection the object still names — stays in the table. See "Iterators" below
and `tests/run/iter_combinators.dt`.

Being excluded from the vtable does **not** make a method uncallable. A provided
method is a generic function over `Self`, so calling it on a trait object
compiles its body at `Self = dyn Trait` rather than dispatching through a slot,
and the calls it makes on `self` go through the table on their own. `d.map(f)`,
`d.max()` and `d.fold(..)` all run
(`tests/run/dyn_excluded_method.dt`, `tests/run/dyn_assoc_bound_method.dt`).
A `where Self.Item: Ord` is then *checked* at that call rather than skipped —
the trait object states what `Item` is, so the question can finally be asked
(`tests/fail/dyn_bound_assoc_unsatisfied.dt`). The one thing still out of reach
is an **associated function**: with no `self` there is no value to carry, and no
instantiation rescues that.

The exclusion has one visible consequence. If a concrete impl writes its own
version of an excluded method, a trait object will not find it — nothing outside
the vtable can — so `dyn Iterator`'s `map` is always `Iterator::map`, even for a
type that overrode it. Rust behaves the same way.

**A `dyn Trait` satisfies `T: Trait`.** A trait object is a bound whose witness
travels with the value, so it meets the bound naming that trait — it *is* the
trait, rather than implementing it:

```
fun total<I: Iterator<Item = Int>>(it: I) -> Int { .. }

var d: dyn Iterator<Item = Int> = Counter { n: 0, max: 3 };
print(total(d));   # 3
```

It satisfies that trait and no other: a `dyn Display` still fails a `T: Ord`
bound. Its associated bindings are part of the type, so they are checked
against the bound's own clauses — a `dyn Iterator<Item = Int>` does not satisfy
`I: Iterator<Item = String>` — and they are what a `where I.Item: Ord`
discharges against. See `tests/run/dyn_bound.dt`,
`tests/fail/dyn_bound_wrong_trait.dt` and
`tests/fail/dyn_bound_item_mismatch.dt`.

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

### Iterators

`for x in it` over a value that is neither an array nor a range requires the
type to implement `Iterator` (`std::iter`, preluded). An array and a range
reach the trait through `xs.iter()` / `(0..n).iter()`, and a `String`'s
characters through `s.chars()` — see "Sources" at the end of this section:

```
@lang("iterator")
pub trait Iterator { type Item; fun next(self) -> Option<Self.Item>; }
```

The loop drives `it.next()` each turn: a `Some(x)` binds `x` and runs the body,
a `None` ends the loop. The iterator is evaluated once and its cursor advances
*in place* — `next` mutates the receiver's own fields (a `pub` type can keep them
private, so the cursor is hidden), which is why an iterator is usually a small
struct with a mutable position:

```
pub struct Counter { n: Int, max: Int }
impl Counter { fun to(max: Int) -> Counter { Counter { n: 0, max: max } } }
impl Iterator for Counter {
    type Item = Int;
    fun next(self) -> Option<Int> {
        if self.n >= self.max { return Option::None; }
        self.n += 1;
        return Option::Some(self.n);
    }
}
for x in Counter::to(3) { print(x); }   # 1 2 3
```

(The iterable is written as `Counter::to(3)` rather than a `Counter { .. }`
literal because a `{` right after the `in` expression starts the loop body —
the same struct-literal restriction `if`/`while` conditions have.)

The gate is **nominal** — the type must implement the `Iterator` trait — but the
`Option` it returns is unwrapped **structurally**, the same `Some`/`None`-by-shape
rule `?` uses for `Ok`/`Err`, so nothing is tied to std's particular `Option`
beyond its two variants. `break` and `continue` work as in any loop; `continue`
re-drives `next()`.

The iterable may also be a **bounded generic** or a **trait object**, not just a
concrete type:

```
fun count<I: Iterator>(it: I) -> Int {   # driven through the `I: Iterator` bound
    var n = 0;
    for x in it { n += 1; }
    return n;
}
var d: dyn Iterator<Item = Int> = Counter::to(5);
for x in d { print(x); }                 # driven through the vtable
```

#### Combinators

`Iterator` carries `map`, `filter`, and `collect` as **provided methods**, so
every iterator gets them for free — an impl fills in `next` and inherits the
rest. The pipeline reads left to right: `map` and `filter` are **lazy** (each
wraps `self` in a small adapter that is *itself* an `Iterator`, so they chain
and only pull as far as they are driven), and `collect` is the **eager** end —
it drains into an array.

```
# square each element, keep those > 3, drain into an array
var xs = Counter::to(6).map(|x| x * x).filter(|v| v > 3).collect();
#   xs == [4, 9, 16, 25]

for y in Counter::to(3).map(|x| x + 100) { print(y); }    # 100 101 102
```

Alongside them: `take(n)` (at most the first `n`), `skip(n)` (all but the first
`n`), `enumerate()` (`(index, element)` pairs), `zip(other)` (walk two iterators
in lockstep, ending with the shorter), `flat_map(f)` (map each element to a
whole iterator and yield those end to end), `fold(init, f)` (reduce left to
right into an accumulator — the eager sibling of `map`), `max()` / `min()`
(the largest or smallest element, or `None` if there is none), `sum()` /
`product()` (combine every element, or `None` if there are none), and
`chain(other)` (this sequence, then `other`'s).

And the short-circuiting consumers: `any(pred)` / `all(pred)` (does any element
pass, does every one), `find(pred)` (the first element that passes) /
`position(pred)` (its index), `count()` (how many are left), and `for_each(f)`
(run `f` on each, for its effect). Alongside them two more lazy adapters,
`take_while(pred)` and `skip_while(pred)`.

```
var sum   = Counter::to(5).fold(0, |acc, x| acc + x);          # 10
var first = Counter::to(100).map(|x| x * x).take(4).collect(); # [0, 1, 4, 9]
var pairs = Counter::to(3).zip(Counter::to(2)).collect();      # [(0,0),(1,1)]
var rest  = Counter::to(5).skip(2).collect();                  # [2, 3, 4]
var flat  = Counter::to(4).flat_map(|n| Counter::to(n)).collect();
#   flat == [0, 0, 1, 0, 1, 2]
var peak  = Counter::to(9).filter(|k| k % 3 == 0).max();       # Some(6)
var both  = Counter::to(3).chain(Counter::to(2)).collect();    # [0,1,2,0,1]
var tot   = Counter::to(5).sum();                              # Some(10)
var seen  = Counter::to(5).any(|x| x == 3);                    # true
var hit   = Counter::to(9).find(|x| x % 7 == 0);               # Some(0)
var n     = Counter::to(9).filter(|x| x % 3 == 0).count();     # 3
var head  = Counter::to(9).take_while(|x| x < 3).collect();    # [0, 1, 2]
var tail  = Counter::to(6).skip_while(|x| x < 3).collect();    # [3, 4, 5]
Counter::to(3).for_each(|x| print(x));                         # 0 1 2
```

**Where each consumer stops is part of what it means**, not an optimisation:
`any` stops at the first element that passes, `all` at the first that fails,
`find`/`position` at the first match. The cursor is left there, so the same
source may be driven on afterwards, and a sequence too long to drain is still
usable when the answer comes early. `count` is the exception — the answer is
only known once the sequence has ended, so it drains.

`take_while` is spent at the first failure and stays spent, which is the whole
difference from `filter`: over `0,1,2,3,0,1` the first yields `[0,1,2]` and the
second `[0,1,2,0,1]`. Its mirror `skip_while` *yields* the element that ended
the skipping rather than dropping it.

`for_each`'s closure returns `Unit`, and there is no discard coercion — a
shorthand closure *is* its expression, so `|x| f(x)` fits only when `f`
returns nothing. `|x| { f(x); }` is the way round it, the trailing `;` doing
the dropping by the ordinary block rule
(`tests/fail/iter_for_each_returns.dt`).

`flat_map` is the one that needed the language rather than the library to move.
Its adapter's element type is `J.Item` — the element of an iterator named only
by a type *parameter* — and a projection over a parameter has to survive
substitution to key the instantiation the call compiles to. The `skip` beside it
needed nothing new.

Making them methods rather than free functions took the per-method object
safety above: `map` (a type parameter of its own) and `filter` (a `Self`-shaped
return) are excluded from a `dyn Iterator` vtable, while `collect` stays
dispatchable, so the trait keeps them *and* stays usable as `dyn`. The closures
are typed by the source's element: `map`'s `|x| ...` sees `x` at the
iterator's `Item` type — including through a *pass-through* adapter, where that
element is a projection over a projection (`Filter<Counter>.Item` is
`Counter.Item` is `Int`), which the checker now collapses so a chain like
`filter(p).map(f)` type-checks. They are ordinary library code — an adapter is a
struct with a `fun(..)` field and an `impl Iterator`, nothing built into the
language.

`max`/`min` are the combinators that need a bound on the *element*: comparing
two elements requires `Self.Item: Ord`, which only a `where` on the method's
signature can say. They are excluded from a `dyn Iterator` vtable for the
reason that clause implies — the bound is about the concrete type the coercion
erased. A trait object calls them anyway: the body is compiled at
`Self = dyn Iterator<Item = ..>`, and that site *can* read `Item` off the type
and ask whether it is `Ord`, which is the question the coercion could not.

`sum`/`product` are the same shape one trait over (`where Self.Item: Add` /
`Mul`), and they are what the operator traits unlocked: before `std::ops` a `+`
on an element had nothing to bound, so reducing a sequence meant spelling the
operation out through `fold`. Both return an `Option` for the reason `max` does
— there is no way to write "the identity element of `T`" — so an empty sequence
answers `None` and the caller decides with `unwrap_or(0)`. `impl Add for String`
is why they are not numeric-only: a sequence of words sums to a sentence.

`chain(other)` is the one that needed a predicate rather than a bound. Its
adapter binds `type Item = I.Item`, so returning the *second* source's element
requires saying that `J.Item` **is** `I.Item` — an equality between two
associated types, not a trait they both satisfy —
which `J: Iterator<Item = Self.Item>` is exactly. Like `map` it carries a type
parameter of its own, so it is excluded from a `dyn Iterator` vtable.

#### `DoubleEnded` and `rev`

Reversing needs an iterator that knows where its *other* end is, and that is a
second trait:

```
use std::iter::{Iterator, DoubleEnded};

pub trait DoubleEnded: Iterator {
    fun next_back(self) -> Option<Self.Item>;
    fun rev(self) -> Rev<Self> { .. }
}
```

`next_back` yields from the far end and advances that end inward, so the two
cursors meet in the middle and one source can be driven from either side, or
both:

```
var all  = Span::of(0, 5).collect();               # [0, 1, 2, 3, 4]
var back = Span::of(0, 5).rev().collect();         # [4, 3, 2, 1, 0]
var mix  = Span::of(0, 4);
mix.next();                                        # Some(0)
mix.next_back();                                   # Some(3)
```

It is the standard library's first **supertrait**, and the reason it has to be
one is `Self.Item`: `Item` belongs to `Iterator`, so only `: Iterator` makes it
`Self`'s here as well. The same edge is what lets `Rev`'s two impls
(`impl<I: DoubleEnded> Iterator for Rev<I>` and the `DoubleEnded` one beside it)
call both `self.inner.next()` and `self.inner.next_back()` when `I` was bounded
only by the sub. Unlike `Iterator` it is not a lang item, so it is imported by
name.

`Map` and `Filter` forward it — a transform does not care which end an element
came from — so a pipeline stays reversible across them. The others do not, each
for a reason of its own: `take`/`skip` would have to know how far the far end is
from a length nobody holds, `enumerate`'s index counts from the front, `zip`
pairs by position, and a `chain` driven from both ends needs its two halves to
agree on where they met. Calling `rev` after one of those is "no method named
'rev'" (`tests/fail/iter_rev_not_double_ended.dt`).

`rev` is excluded from a `dyn DoubleEnded` vtable for `filter`'s reason (it
returns a `Rev<Self>`), which does not stop a trait object being reversed by a
`<I: DoubleEnded>` function — the body is monomorphised at
`Self = dyn DoubleEnded` like any other instance.

#### Sources: `xs.iter()`, `(a..b).iter()` and `s.chars()`

Everything above wraps an iterator, and an array and a range are *not* ones:
`for x in xs` and `for i in 0..n` reach them by a desugaring, not through the
trait. `std::iter` ships the seeds that connect them, and the one that connects
a `String`:

```
xs.iter()        # -> ArrayIter<T>, Item = T
(0..n).iter()    # -> RangeIter,    Item = Int
s.chars()        # -> CharIter,     Item = Char
```

All three are `Iterator` *and* `DoubleEnded`, so the whole vocabulary applies:

```
var doubled = [1, 2, 3].iter().map(|v| v * 2).collect();     # [2, 4, 6]
var evens   = (0..10).iter().filter(|v| v % 2 == 0).count(); # 5
var down    = (0..5).iter().rev().collect();                 # [4, 3, 2, 1, 0]
var tail    = [1, 2, 3].iter().rev().take(2).collect();      # [3, 2]
var first3  = "héllo".chars().take(3).collect();             # [h, é, l]
```

The call is required: an array or a range still has no `map` of its own
(`tests/fail/iter_array_needs_iter.dt`,
`tests/fail/iter_range_needs_iter.dt`), and `for x in xs` keeps its desugaring
rather than routing through `iter()`.

**An `ArrayIter` holds the array, not a copy and not a borrow** — ducktape has
none to hold — so a change underneath is visible, which is the rule
`for x in xs` already followed by re-reading the length each turn. What the
iterator carries is how far in it has come from *each end*, never a remembered
end index, so:

- an element pushed ahead of the front cursor is still yielded;
- popping ends the walk early rather than reading off the end;
- no interleaving of the two can produce an out-of-bounds read.

A `Range` is a value, so a `RangeIter` shares nothing and cannot be disturbed;
`var r = 0..3;` seeds as many identical walks as it is asked for. It holds one
**exclusive** bound rather than the range, which is where `a..b` and `a..=b`
stop being two things — `(0..4).iter()` and `(0..=3).iter()` are the same
sequence, and an iterator remembering which spelling it came from would be
carrying a distinction the sequence does not have.

**A `CharIter` holds the string and two byte offsets**, and it *does* snapshot
its end — the opposite of `ArrayIter`, for a reason that is the objects' rather
than a preference: a `String` is interned and immutable, so there is nothing
underneath an offset that could change and nothing to re-read. It is the one
source that has to decode to move: forwards by the width the character's own
value implies (a scalar value determines its encoding), backwards by the
boundary the encoding tags, which is the single native the reverse direction
needs. `s.chars().rev()` therefore costs what the forward walk costs, rather
than building an array to turn round.

All three live in `std::iter` rather than in `std::array` / `std::string`, and
the import graph decides it rather than taste: `collect` needs `push`, so the
edge runs `iter → array` and cannot also run the other way; `std::cmp` imports
`std::string`, so `std::string` cannot import `std::iter` either. The visible
cost is that `std::string`'s `pad_*` lang items cannot reach the walk and count
characters with a native of their own.

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
pub struct Point { pub x: Int, pub y: Int }  # importable, both fields exposed
fun helper() -> Int { 1 }                    # private: `use m::helper;` is an error
```

A struct's **fields carry their own visibility**, and it too is private by
default: a `pub struct` is importable, but each field is readable, assignable,
constructible, and matchable from another module only if it is written `pub`.
Within the module that defines the struct every field is reachable regardless —
visibility is a module boundary, never an intra-file one — so a type can keep an
invariant behind a private field and expose it only through its methods:

```
pub struct Counter {
  n: Int,          # private: only this module touches the cursor
  pub limit: Int,  # public
}
# elsewhere: `c.limit` reads, but `c.n` and `Counter { n: 0, .. }` are errors,
# as is a pattern `Counter { n, .. }` — the field is named either way.
```

Enum variants have no field-level visibility: a `pub enum`'s variants and their
payloads are as visible as the enum itself. `pub` on a tuple-struct field
(`struct P(pub Int, Int)`) works the same way as on a named one.

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
use std::io;
use std::string;

io::print(string::concat(parts));   # `print` and `concat`, each named
                                    # through its module without an import
```

(The primitives' operations — `s.len()`, `xs.push(v)` — are *methods* now, told
apart by their receiver's type, so a qualifier is for the free functions and
user modules that remain, not for those.)

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
which is worth knowing before writing your own method on a primitive: `use
std::array;` reaches `std::option` (that is what `pop` returns), and so
`std::fmt` and `std::cmp` beyond it, and `std::string` beyond *that* — so every
impl those modules ship is visible too, and since milestone 40 that includes
inherent methods on the primitives (`impl<T> [T]`, `impl String`, `impl Char`,
`impl StringBuf`). Importing `std::array` therefore also means you cannot add
your own inherent `[T]` method of a name it already spends (`len`, `push`, …).

That chain is why the direction of a std module's own imports is a design
decision rather than bookkeeping: what an import costs its dependents is the
*impls* the imported module ships, not its size. `std::cmp` imports
`std::string` and `std::char` for `impl Ord for String` / `impl Ord for Char`,
and the cost it passes on is those modules' own `impl String` / `impl Char` —
one type's methods each, on a type the importer did not itself define, rather
than a trait impl coherence could take away.

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
bodyless function bound to C (see "Native functions" below); `std::assert`,
`std::cmp`, `std::convert`, `std::map`, `std::ops`, `std::option`, `std::result`,
`std::sort` and `std::text` need none, `std::io` and `std::panic` are nothing
but, and `std::array`, `std::string`, `std::strbuf`, `std::char` and
`std::hash` are the mixed case — a handful of natives, and every other function
written on top of them in ducktape.

**There is a small prelude.** Every program implicitly imports `Option`
(`std::option`), `Result` (`std::result`), `Ord` (`std::cmp`), `Display`
(`std::fmt`), `Iterator` (`std::iter`) and the six operator traits
(`std::ops`), plus `std::string` for its format-spec helpers — the vocabulary
types and the lang items, so a construct whose meaning the compiler resolves
(`"{v}"`, `a < b`, `a + b`, `for x in it`, a `{v:>8}` spec) works without an
import the user never wrote. The prelude is lowest priority: a module that defines its own `Option`,
or imports a different `Ord`, keeps it — the prelude's binding steps aside
silently. **`print` is *not* in the prelude** — it is an ordinary function tied
to no syntax, so `use std::io::print;` is still required. Everything else in
std is imported explicitly as before:

```
use std::cmp::{max, min, clamp};   # Ord itself is preluded; these are not
```

A std module is an ordinary module in every other respect: it is deduplicated,
takes part in the dependency graph, honours `pub`, and is loaded **only if
some `use` names it** — a program that ignores std pays nothing. Naming a std
module that does not exist is an error listing the ones that do.

### `std::assert`

```
pub fun assert(condition: Bool)                       # "assertion failed"
pub fun assert_with(condition: Bool, message: String)
pub fun assert_else(condition: Bool, message: fun() -> String)
pub fun assert_eq<T: Display>(left: T, right: T)      # "assertion failed: 1 == 2"
pub fun assert_ne<T: Display>(left: T, right: T)
```

Five wrappers over `std::panic`, so what the module actually decides is shape
rather than behaviour.

**An assertion cannot name itself.** Rust's `assert!` is a *macro* so that a
failure can quote the source text back — `assertion failed: a == b`, copied
from the call. ducktape has no macros, and a function receives a `Bool`, not
the expression that produced it, so `assert`'s message is the fixed string and
can never be more. `assert_with` is the way round it, and that is exactly the
`unwrap`/`expect` split one section down, forced by the same absence.

**Arguments are evaluated**, so `assert_with(ok, "index {i} of {n}")` builds
that string on the passing calls too — the cost a macro would have hidden.
`assert_else` takes a closure instead, the shape `unwrap_or` / `unwrap_or_else`
already established for the same reason.

**`assert_eq` is bounded and `==` is not.** The comparison needs nothing:
`==` lowers to `OP_EQ`, which reads no static type and walks any pair of values
structurally, so two values of a type with no impls at all can be compared. The
whole `T: Display` bound is bought by the failure *message* — the runtime can
render any value, but only to stdout, and `panic` takes a `String` that nothing
structural produces. So it is a consumer of equality that specifically does not
motivate an `Eq` trait (see `roadmap.md` item 2): it wants a way to print, and
it has one (`tests/fail/assert_eq_needs_display.dt`).

**The module is separate from `std::panic` for one reason**: `Display` brings
`std::fmt`'s impls, impl visibility is transitive, and a program that only
wanted `panic("...")` must not inherit them — nor coherence's refusal to let it
write its own `impl Display for Int`. Same argument `std::cmp` makes about
where `impl Ord for String` belongs; the dependency points at the impl-poor
module, so `assert` imports `panic` and never the reverse.

There is **no conditional compilation**, so nothing here vanishes from a
release build: an `assert` in a hot loop is a branch in a hot loop
(`tests/run/std_assert.dt`).

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

**`String` is ordered by a trait, not by a built-in opcode** — but since
milestone 38 the operator reaches that trait: `a < b` on a String desugars to
`a.cmp(b) < 0`, so comparing two Strings is `a < b`, `a.lt(b)`, or `max`/`min`/
`clamp`, all reaching `impl Ord for String` (`tests/run/string_ord.dt`):

```
use std::cmp::{Ord, max};
print(max("apple", "pear"));      # pear
print("Zebra" < "apple");         # false — byte order, not locale
```

The comparison is by *bytes*, which for well-formed UTF-8 is code-point order:
a multi-byte sequence's lead byte is above every ASCII byte. A prefix sorts
before what extends it, and there is no locale, case-folding or normalisation.

The interesting part is what interning does *not* buy here. `==` on a String is
a pointer compare because two equal strings are one object in the intern table
— but pointer *order* is allocation order, so ordering gets nothing from the
table beyond the equal case, and has to walk the bytes. That walk is the String
method `compare` (`a.compare(b)`), and it has to be a native for the same reason
`push` is: the finest handle ducktape has on a String's contents is `slice`, and
comparing two one-byte slices would need the ordering being defined.

`Char` is ordered the same way, and by the same rule about where the impl
goes: `std::cmp` imports `std::char` for it. That one needs no native at all —
`c.code()` hands the comparison two Ints, and `<` on an Int is an opcode —
which is the difference between ordering a character and ordering a string of
them.

The impl lives in `std::cmp`, beside the trait, so `std::cmp` imports
`std::string` (and `std::char`) rather than the other way round. That direction
is deliberate: impl visibility is transitive through `use`, so importing them
brings their `impl String` / `impl Char` (the methods `compare` and `code` live
on) into every `use std::cmp::…` — one type's methods each, and nothing a
program did not already reach through `std::cmp`.
Putting the impl in `std::string` would have handed a program that only wanted
`len` every `Ord` impl — and with them coherence's refusal to let it write
its own `impl Ord for Int`. **An import's cost is measured in impls, not in
code.** `impl Display for String` living in `std::fmt` sets the same precedent.

`Float` is ordered by IEEE comparison, which is *not* a total order: every
`<`/`>`/`==` involving a NaN is false. `Ord` promises a total order, so the impl
decides where NaN goes — **NaN sorts after every real number, and all NaNs are
equal to each other** (`self != self` is the NaN test, since only NaN is unequal
to itself). So `max(nan, x)` is NaN whichever argument the NaN is, and a
`[Float]` with a NaN in it has a defined sort (`tests/run/float_nan.dt`). The
sign bit and payload are ignored, so a `-NaN` and a `+NaN` are one order —
deliberately coarser than Rust's `total_cmp`.

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

### `std::ops`

```
pub trait Add { fun add(self, other: Self) -> Self; }   # +
pub trait Sub { fun sub(self, other: Self) -> Self; }   # -
pub trait Mul { fun mul(self, other: Self) -> Self; }   # *
pub trait Div { fun div(self, other: Self) -> Self; }   # /
pub trait Rem { fun rem(self, other: Self) -> Self; }   # %
pub trait Neg { fun neg(self) -> Self; }                # unary -
```

What `std::cmp` is to `<`, this is to `+`: a trait decides what the operator
means, so `a + b` on a non-numeric operand is the call `a.add(b)`, rewritten by
the checker (`tests/run/ops_operators.dt`).

```
struct V2 { x: Int, y: Int }
impl Add for V2 {
    fun add(self, other: V2) -> V2 {
        return V2 { x: self.x + other.x, y: self.y + other.y };
    }
}
impl Neg for V2 { fun neg(self) -> V2 { return V2 { x: -self.x, y: -self.y }; } }

var c = V2 { x: 1, y: 2 } + V2 { x: 10, y: 20 };   # (11, 22)
var d = -c;                                        # (-11, -22)

fun twice<T: Add>(v: T) -> T { return v + v; }     # and so a generic can add
print(twice(3));                                   # 6      — via impl Add for Int
print(twice("ab"));                                # abab   — via impl Add for String
```

Three things are worth knowing about the shape:

- **A numeric operand never reaches for these.** `1 + 2` is still `OP_ADD` — no
  import, no frame, no impl lookup — exactly as `1 < 2` stays an opcode and
  `"{1}"` stays the VM's own rendering. The impls `std::ops` ships for `Int`,
  `Float` and `String` are not what makes `1 + 2` work; they exist so a generic
  bounded `T: Add` can instantiate at a primitive, and each of their bodies is
  the built-in path (`return self + other;` on two Ints is the opcode, so it
  does not call itself).
- **They are homogeneous**, `Self` on both sides. There is no `Rhs` parameter
  and no `Output` associated type, so `V2 * Float` has no spelling: the operator
  would have to select an impl by its right operand, and selection by argument
  type exists only through the written `Meters::from(x)` / `Into::<U>::into(x)`
  forms. A mixed `Cents + Int` is reported as an argument mismatch on the
  rewritten call.
- **None is object-safe** (`other: Self`, and `-> Self` for `Neg`), so they are
  bounds and never a `dyn Add` — the position `Ord` is in, and the same rule:
  object safety is demanded only where `dyn` is written.

All six are preluded, so `impl Add for Point` and a `T: Add` bound need no
`use`. As with `Display` and `Ord`, that also means the impls for the primitives
are always visible, so a program cannot write its own `impl Add for Int` —
coherence rejects the pair.

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

An **impl method** may carry an attribute too, so a primitive's operation can be
spelled `s.len()` rather than as a free `string::len(s)`:

```
impl String {
    @native("string_len") fun len(self) -> Int;
}
impl<T> [T] {
    @intrinsic("array_len") fun len(self) -> Int;   # lowers to OP_LEN inline
}
```

`self` is an ordinary parameter to the C function — the same value it would have
received as the first argument of the free-function form — so nothing below the
signature changes: a native method dispatches through the same `OP_CALL`, takes
an ordinary global slot, and slots into a `dyn Trait` vtable like any other
method; an intrinsic method lowers to its opcode at the call site. A *trait*
method (in a `trait` declaration) still cannot be native — its default body is
generic over `Self` — so the attribute is accepted on a top-level `fun` and on
an impl method only. **Since milestone 40 the standard library uses this**: the
primitive modules spell their operations as methods (`s.len()`, `xs.push(v)`,
`c.code()`), with constructors as associated functions (`StringBuf::new()`,
`Char::from_code(n)`). What stays a free function is the exceptions the design
forces — a *builder* whose receiver is a collection not the primitive
(`std::string`'s `join`/`concat`/`from_chars`), a lang item the compiler
desugars into (`pad_*`, `float`), a private raw native or intrinsic (`array`'s
`pop_last`, `iter`'s `range_start`/`range_stop` — an impl method has no
visibility control, so anything that must stay private stays a free function),
and the one length `std::cmp` needs but cannot reach as a method without closing
a dependency cycle.

The intrinsic tier is what makes an existing opcode *nameable*. `OP_LEN`,
`OP_RANGE_START` and `OP_RANGE_STOP` were all emitted by a `for` desugaring
before they were declared anywhere; giving each a declaration surface is what
let `xs.len()` and then `std::iter`'s two sources be written in ducktape. The
constraint on which opcodes can be exposed this way is mechanical: no operand
bytes, popping exactly the declared parameters and pushing exactly the declared
return.

Nothing restricts natives to `std/` — a user module may declare one too. The
registry is closed, so the only names available are the ones the binary already
provides, and the *decision* about what C exposes stays in C. Restricting the
attribute by module would add a rule without adding a guarantee.

### Lang items (`@lang`)

A handful of constructs desugar to a std definition the user never names: `"{v}"`
on a non-primitive calls `Display`'s `to_string`, `a < b` on a non-numeric calls
`Ord`'s `cmp`, `a + b` calls `Add`'s `add`, a `{v:>8}` spec calls
`pad_*`/`float`. The compiler has to know *which* definition each is, so the
standard library marks them with a third attribute, `@lang("…")`:

```
@lang("display") pub trait Display { fun to_string(self) -> String; }
@lang("ord")     pub trait Ord     { fun cmp(self, other: Self) -> Int; }
@lang("add")     pub trait Add     { fun add(self, other: Self) -> Self; }
@native("fmt_float") @lang("float") pub fun float(v: Float, p: Int) -> String;
```

The six operator traits are one key each (`add`/`sub`/`mul`/`div`/`rem`/`neg`),
and the key is the *method* name rather than the trait's: a marker names the
operation it stands for.

Unlike `@native`/`@intrinsic`, `@lang` is a **marker**: it does not replace the
body, so it sits on an ordinary trait, enum, or top-level function — and it can
share a definition with a body attribute, which is why `float` carries both. The
key names the item; an unknown key inside std is a compile error.

`@lang` is for the standard library. In a user module it is **inert** — a user
cannot claim a lang item and so cannot change what `"{v}"` or `a < b` mean — but
it is not rejected, because a std file run directly (`ducktape std/fmt.dt`)
carries the same markers and must still work as an ordinary module. (Putting
`@lang` on something it cannot mark — a struct, a method — is a parse error, the
one placement it is refused everywhere.)

### `std::io`, `std::array`, `std::string`, `std::strbuf`

The primitive operations are **methods** (milestone 40); a free-function line is
a deliberate exception, marked below.

```
std::io      print<T>(value: T)                     # @native, free (see below)

impl<T> [T]  self.len() -> Int                      # @intrinsic (OP_LEN)
             self.push(value: T)                    # @native
             self.pop() -> Option<T>
             self.first() -> Option<T>
             self.last() -> Option<T>
             self.is_empty() -> Bool
             self.clear()
             (std::array also: private free `pop_last<T>(xs)`, the raw @native)
             self.iter() -> ArrayIter<T>            # lives in std::iter
             self.sort_by(cmp: fun(T, T) -> Int)    # lives in std::sort
             self.sort()                            # std::sort, needs T: Ord
             self.is_sorted() -> Bool               # std::sort, needs T: Ord
             self.lower_bound(target: T) -> Int     # std::sort, needs T: Ord
             self.binary_search(target: T) -> Option<Int>   # std::sort, T: Ord

impl Range   self.iter() -> RangeIter               # lives in std::iter
             (std::iter also: private free `range_start`/`range_stop`,
              the two @intrinsics that read a range's bounds)

impl String  self.len() -> Int                      # @native, in bytes
             self.slice(from: Int, to: Int) -> String         # @native
             self.compare(other: String) -> Int     # @native
             self.chars() -> CharIter               # lives in std::iter
             self.repeat(n: Int) -> String
             (std::iter also: private free `char_at`/`prev_boundary`, the two
              @natives a character walk needs; std::string keeps a private
              free `char_width`, the @native the pad_* lang items count with)
             std::string free: join(parts: [String], sep: String) -> String
                              concat(parts: [String]) -> String
                              from_chars(cs: [Char]) -> String
                              pad_start/pad_end/pad_center(s, width, fill)  # lang items

impl StringBuf  StringBuf::new() -> StringBuf        # @native (associated)
             self.push(s: String)                   # @native
             self.push_char(c: Char)                # @native
             self.push_int(n: Int)                  # @native
             self.len() -> Int                      # @native
             self.clear()                           # @native
             self.build() -> String                 # @native
             self.is_empty() -> Bool

impl Char    self.code() -> Int                     # @native
             Char::from_code(n: Int) -> Char        # @native (associated)
             self.is_ascii/is_digit/is_lower/is_upper() -> Bool
             self.is_alpha/is_alnum/is_whitespace() -> Bool
             self.to_upper() -> Char                # ASCII only
             self.to_lower() -> Char                # ASCII only
             self.to_digit() -> Int

std::fmt     float(value: Float, precision: Int) -> String   # @native, free (lang item)
```

`print` stays a free function because it is a general operation over any `T`,
not a method on one type; the `pad_*` and `float` functions stay free because
they are lang items the `{v:>8}` / `{f:.3}` format spec desugars into, so their
meaning cannot depend on what a program imported; `join`/`concat`/`from_chars`
stay free because their receiver is a `[String]` / `[Char]`, not a `String`.

**`print` is not a builtin and is not in scope by default** — `use
std::io::print;` is a real import of a real module, and forgetting it is an
ordinary "cannot find 'print' in this scope" error. It is deliberately kept out
of the prelude (which does cover `Option`/`Result`/`Ord`/`Display`): `print` is
a plain function, not tied to any syntax, so it stays explicit.

`String`'s `slice` and `std::fmt::float` are the std operations that can fail
without a `Result`: a native reports a runtime error by setting `ctx->error`,
and the VM raises it at the call site, exactly as `std::panic::panic` does.

**An array grows.** `push` appends, reallocating the buffer behind the array
when it is full; an array literal starts exact-fit, so `[1, 2, 3]` and `[]` are
both ordinary starting points:

```
var xs: [Int] = [];
xs.push(1);
xs.push(2);
print(xs.pop().unwrap());   # 2
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

var b = StringBuf::new();
for word in words {
    b.push(word);
    b.push(" ");
}
print(b.build());
```

The operations are methods on `StringBuf`, with `new` the associated
constructor. A method resolves on its receiver, so `b.len()`, `s.len()` and
`xs.len()` coexist unqualified — the collision the free-function spelling once
had to dodge (`buf_len`, `push_str`) and later disambiguate with a qualifier is
gone, since a method names the operation and the receiver names which one.

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

Seven of `StringBuf`'s methods are written in C — existing (the associated
`new`), growing (from a String, a Char or an Int's digits), emptying, its
length, and becoming a String — and `is_empty` is ducktape on `len`. `push_int`
puts a number's digits in without interning a `"{n}"` String to carry them, and
`clear` drops the length to zero while keeping the capacity, so one buffer can be
reused across a loop. `std::string`'s `repeat` method and its free `join`,
`concat` and `from_chars` are the String-shaped conveniences on top, built
through the buffer, the same split `std::array` makes; `repeat` is the one that
shows what the buffer buys — it copies bytes straight in, allocating no String
per copy. `std::string` reaches only `std::strbuf`, which imports nothing and
ships only methods on its own `StringBuf`, so importing `std::string` hands a
program no impls for a type it did not itself name beyond that one — the property
that makes it safe for `std::cmp` to depend on it for `impl Ord for String`. What
`std::string` must *not* reach is `Option` or `std::array`, which would close the
cycle `string → option → cmp → string`; the buffer is a pure leaf below it, so
the edge to it is free.

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
two apart.** `s.len()` counts *bytes*, `s.slice(..)` cuts at *byte* offsets, and
`s.compare(..)` walks bytes; `s.chars()` is the only crossing:

```
var s = "héllo";
s.len();                        # 6 — bytes
s.chars().count();              # 5 — characters
s.chars().collect();            # [h, é, l, l, o]
from_chars(s.chars().collect()) == s;   # true
```

**`chars` is a walk, not a conversion** (milestone 61): it answers a `CharIter`,
the `std::iter` source described under "Sources" below, so `count`, `rev`,
`take` and every other combinator apply and the `[Char]` is what `collect` is
for. Nothing is decoded until something pulls.

There is deliberately **no `char_at(s, i)`**. The index would be a byte offset,
a byte offset is not a character position, and offering that spelling would
make confusing the two the default rather than the mistake. `CharIter` holds
byte offsets — it has to — and keeps them private for that reason. Going back
is `from_chars`, through a `StringBuf`, which `push_char` is what makes
possible.

Walking is a **runtime error if the string is not valid UTF-8**, which a
program can provoke: `slice` cuts at byte offsets, so it can halve a multi-byte
sequence. A String is a byte string; only a `Char` promises to be a character.
Because the walk is lazy the error belongs to the `next()` that reaches those
bytes rather than to `chars()` itself, so a pipeline that stops earlier
(`s.chars().take(2)`) never provokes it. The `pad_*` lang items still reject a
halved string outright: they count characters with a native of their own, in
one validating pass.

Everything else in `std::char` is ordinary ducktape over `code`/`from_code`
(methods on `Char`, `from_code` the associated constructor) — the one thing a
Char cannot say about itself is its number, and once it can, every
classification is a range test and every case conversion is an addition. **The
classifications are ASCII-only**: `'é'.is_alpha()` is false and `'é'.to_upper()`
is `'é'` unchanged. Full Unicode case mapping is a table, not a range test, and
shipping a range test under that name would be right for English and quietly
wrong elsewhere.

`impl Ord for Char` (in `std::cmp`, beside the trait) is code-point order, so
`'Z'` sorts before `'a'` — the same order `impl Ord for String` gives, since
UTF-8 byte order and code-point order agree. As with `String`, ordering is the
trait and not the operator: `'a' < 'b'` is still "comparison requires numeric
types".

### `std::text`

`std::text` is searching, splitting, trimming and parsing, written entirely in
ducktape on the primitives `std::string` and `std::char` already offer. Its
operations are **methods on `String`** (milestone 41), a second `impl String`
one module up from the leaf's — so importing the module is all it takes and no
item is ever named:

```
use std::text;

"hello".starts_with("he");    # true
"hello".ends_with("lo");      # true
"hello".find("l");            # Some(2) — a byte offset
"hello".contains("ell");      # true
"a,b,,c".split(",");          # ["a", "b", "", "c"]
"  hi there  ".trim();        # "hi there"
"-42".parse_int();            # Some(-42);  "12x".parse_int() is None
```

Two inherent impls for one type coexist: `std::string`'s `impl String` and this
one declare disjoint method names, and neither is a trait impl, so coherence has
no say — method resolution finds each name in whichever visible impl declares
it. The module boundary the cycle below forces is invisible at the call site.

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
`Option`, taken to the point where the split is *forced*. `std::text` uses
`s.len()` (bytes) and `cs.len()` (elements) side by side, told apart by their
receiver's type — the collision that once made this the showcase for module
qualifiers is what methods retired.

Within the module the two views of text stay apart, the milestone-26 way:

- **A *position* is a byte offset**, because a position is one you will `slice`
  at. `find` and `split` are `slice`+`==` and never inspect a byte on its own —
  so `"héllo".find("llo")` is `Some(3)`, and that 3 feeds straight back into
  `slice`. The cost is that `find` slices and interns a candidate substring at
  every position (O(*n·m*) allocations), the honest price of a String whose only
  reader is `slice`: there is no `byte_at`, so a window has to be cut out to be
  compared.
- **A *character* is classified by its value**, so `trim` and `parse_int` cross
  to the `chars` view — whether something is a space or a digit is a fact about a
  character, not a byte, and an ASCII space is a single byte only by luck of the
  encoding. Both say `.chars().collect()`: they want the characters by index
  from either end, and the array a walk builds on request is what that needs.

`starts_with`/`ends_with` need neither a position nor a classification, so they
are the pure `slice`+`==` ones. The empty pattern is read consistently
everywhere: `s.starts_with("")` and `s.find("")` both succeed at 0, and
`s.split("")` returns `s` whole in a one-element array (there is no position at
which `""` is *not*, so the loop that finds one would never end). `split` yields
one more piece than there were separators, so a leading, trailing, or doubled
separator each produces an empty piece. `parse_int` accepts an optional leading
`+`/`-` then one or more digits and nothing else — an empty string, a bare sign,
or a stray character is `None`, not a partial parse — and does **not** detect
overflow: a value past what an `Int` holds wraps.

### `std::sort`

Ordering an array, and using one that is ordered. Methods on `[T]` (a second
inherent impl, the way `std::text`'s are on `String`), so `use std::sort;` is
all it takes:

```
use std::sort;

var xs = [5, 3, 9, 1];
xs.sort();                          # [1, 3, 5, 9] — in place, needs T: Ord
xs.sort_by(|a, b| b.cmp(a));     # [9, 5, 3, 1] — any order, no bound
xs.is_sorted();                     # false (this one is descending now)

var v = [10, 20, 20, 30];
v.binary_search(20);                # Some(1) — the first of the equals
v.binary_search(25);                # None
v.lower_bound(25);                  # 3 — where 25 would be inserted
```

- **`sort_by` is the primitive and `sort` the convenience**, the fork
  `std::iter`'s `max` settled the same way: the closure form takes the order as
  an argument, the `Ord` form takes it from the element. `sort` and the searches
  live in an `impl<T: Ord> [T]` block, so an array of an unordered element does
  not *have* them and the diagnostic names the obligation
  (`tests/fail/sort_needs_ord.dt`).
- **Sorting is in place**, because a `[T]` is a reference and the caller holds
  the array being rearranged. A sorted *copy* needs no second method: it is
  `var ys = xs.iter().collect(); ys.sort();`.
- **The sort is stable**, and that promise picks the algorithm: a bottom-up
  merge sort keeps it unconditionally, where an in-place quicksort would trade
  stability away and take quadratic time on already-sorted input. The price is
  one array of scratch space. Stability is what lets a program sort by one key
  and then another and mean it (`tests/run/sort.dt` observes both).
- **`lower_bound` is the primitive of the two searches** — an insertion point
  exists whether or not the element does, and `binary_search` is one comparison
  past it. Both require `self` to be sorted; on an array that is not, the answer
  is unspecified but never an out-of-bounds read.
- **"Found" means `cmp` answers 0, not `==`.** The two can disagree — a struct
  ordered on one of its fields, a case-insensitive comparator — and it is the
  order's answer a search can honour, since the order is what put the elements
  where they are. This is also why the absence of an `Eq` trait costs the module
  nothing: the notion of equality a search needs was already written as `Ord`.

`sort_by` is a **native**, and the only one that runs the program's own code:
its comparator is a value the caller wrote, so each comparison is a call back
out of C into ducktape (`runtime.md` → "Calling ducktape from a native"). The
visible consequences are two. A comparator that fails — a panic, a bad index —
fails the sort, reported where it happened. And a comparator that *resizes* the
array being sorted is refused: the native orders a snapshot and writes it back
in one go, so finding the length changed it declines rather than scatter an old
sequence over a new array, leaving the array exactly as the comparator made it
(`tests/fail_run/sort_resize.dt`). Reading that array, or sorting another one,
is fine.

The `Ord` half above it needed no language change at all; `std::sort` is a
module of its own rather than more of `std::array` for `std::text`'s
reason applied one type over — `std::array` is what an array cannot say about
itself, while an order is a policy written on top. What makes the module
possible is a decision `std::cmp` made earlier: it reaches an array's length
through a private `@intrinsic` instead of importing `std::array`, so `sort →
cmp` closes no loop. Unlike `push` and `len` — which every program has through
the prelude's `std::iter → std::array` chain — `sort` is opt-in, and the reason
is that a visible std method on `[T]` **silently wins** over a program's own
method of the same name (see "Not yet implemented"). Opt-in is what leaves the
name to a program that never asked for this module.

### `std::hash`

Turning a value into an Int a table can index with.

```
use std::hash::{Hash, Hasher, hash_of};

hash_of(7);                 # an Int, spanning the whole range — negatives too
hash_of("alpha");
hash_of((1, "a"));
hash_of([[1], [2]]);

impl Hash for Point {
    fun hash(self, h: Hasher) {
        self.x.hash(h);
        self.y.hash(h);
    }
}
```

`Hash` ships for `Int`, `String`, `Bool`, `Char`, `[T: Hash]` and
`(A: Hash, B: Hash)`. `Hasher` offers `new`, `write_int`, `write_string`,
`write_bool`, `write_char` and `finish`; `hash_of(v)` is the whole of a hash in
one call, and is what a caller that is not itself an impl wants.

- **`hash` writes into a `Hasher` rather than returning an Int**, and that is
  forced by the language rather than borrowed from Rust. With no bitwise
  operator (see "Expressions") a mixing function cannot be written in ducktape
  at all; the one an implementor *could* write is `h * 31 + x`, which is the
  weak one. Writing into an accumulator moves the mixing into a single object
  that reaches C for it, so an impl only ever says *which parts of me matter*.
  `Hasher` is to `Hash` what `StringBuf` is to building text.
- **A `Hasher` is passed, not returned.** A struct is a heap object behind a
  handle, so a callee assigning to `self.state` assigns to the caller's hasher —
  the same reason `xs.push(v)` needs no way to spell "by reference".
- **The contract is one-sided and nearly unbreakable**: values that are `==`
  must write the same bytes. Since `==` is structural and cannot be overridden,
  any impl reading only the value's own contents satisfies it by construction.
- **A `String`'s hash is a field read** — the heap hashed the bytes when it
  interned them — so the type whose hash would otherwise be a walk is the
  cheapest one here.
- **`impl Hash for [T]` writes the length first.** Without it, `[[1], [2]]` and
  `[[1, 2]]` would write the same sequence and hash alike.
- **There is deliberately no `impl Hash for Float`**: `NaN != NaN`, so such a key
  could never be found again in a table that resolves collisions with `==`, and
  the only Int a Float can reach is a truncating `as`. `Ord for Float` had to
  place NaN *somewhere* because an order is total; a table may simply decline
  the key.
- Two natives, `hash_mix` and `hash_string` — exactly the two steps ducktape
  cannot take. Not preluded.

### `std::map`

A hash map and the set written on it.

```
use std::map::{HashMap, HashSet};

var m: HashMap<String, Int> = HashMap::new();
m.insert("one", 1);            # None — nothing was displaced
m.insert("one", 11);           # Some(1) — the value it replaced
m.get("one");                  # Some(11)
m.contains_key("two");         # false
m.remove("one");               # Some(11); again -> None
m.len(); m.is_empty(); m.clear();

var ks = m.keys();  ks.sort();    # [K], in no particular order until sorted
var vs = m.values();
for pair in m.iter() { ... }      # (K, V), an ordinary Iterator

var s: HashSet<Int> = HashSet::new();
s.insert(4);                   # true — newly added
s.insert(4);                   # false — already there
s.contains(4); s.remove(4); s.to_array();
```

`K` needs `Hash` and nothing else.

- **A map needs no `Eq`.** Collision resolution compares keys with `==`, which
  is a structural primitive with no trait behind it, so `k == key` type-checks
  on a `K` bounded only by `Hash`. The consumer the roadmap was holding `Eq`
  back for turns out not to want it.
- **Open addressing with linear probing**, the shape the VM's own intern table
  already has. A removed slot becomes a **tombstone** rather than empty,
  because a probe that stopped there would miss keys that had walked past it.
- **Capacities are primes.** With no `&`, the mask trick that forces
  power-of-two capacities elsewhere is unavailable, so the reduction is `%` and
  a prime is its natural partner. Growth is at 3/4 load, measured including
  tombstones so an insert/remove workload cleans rather than merely grows.
- **Iteration order is unspecified** — slot order, which is hash order, and it
  changes when the table rehashes. Sort if you need an order; a test that prints
  a map without sorting is testing the hash function.
- **`iter()` holds the slot array, not the map**, so an insert that rehashes
  mid-walk leaves the walk on the old array rather than corrupting it. That is
  `CharIter`'s choice rather than `ArrayIter`'s, and it is forced: a rehash
  moves every entry, so an index into the old table means nothing in the new.
- **`HashSet<T>` is a `HashMap<T, Bool>`** whose values are never read — one
  wasted Bool per entry, traded against a second copy of the algorithm, since
  nothing can be generic over "a payload or none".
- No natives and no language change; `std::hash` is the only thing it needed.
  Not preluded.

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
| indexing a `String` by character (`s[i]`) | there is none: `s.chars()` walks, because a byte offset is not a character position. `s.chars().collect()` is the array, and `s.chars().skip(i).next()` is the one character |
| a `String` that is guaranteed valid UTF-8 | it is a byte string — `slice` cuts at byte offsets, so a walk over a halved sequence reports a runtime error when it reaches it (`chars()` itself never does: it is lazy) |
| a *dynamic* width or precision in a format spec (`{v:>{n}}`) | the width and precision in a `{v:>8}` / `{f:.3}` spec are literals; a runtime value there has no spelling. The spec itself is sugar for `std::string::pad_*` / `std::fmt::float` (milestone 35) |
| casting a `dyn Trait` back to its concrete type | no downcast; the coercion is one-way |
| a trait's type arguments at a *bare* method call | the expected type breaks the tie between two impls of one generic trait, and pins an impl parameter the receiver cannot reach (`impl<T, U: From<T>> Into<U> for T`); with no expected type the first impl wins — or, where the parameter was only pinnable that way, no impl applies at all. The trait-qualified spelling (`Into::<Fahrenheit>::into(c)`) settles it explicitly without an expected type |
| disambiguating a qualified selection whose *argument is itself unresolved* (`Steps::from(None)`) | the argument (for `from`) or the receiver (for a trait-qualified `into`) must type on its own to choose the impl, so a value that would need the impl chosen first cannot be disambiguated |
| a bound naming a *later* type parameter (`fun f<U: Into<T>, T>`) | "unknown type: T" — bounds resolve left to right |
| an equality binding between two *projections* (`J: Iterator<Item = I.Item2>` where both sides are abstract) | works, and is compared exactly — but only because a projection over a parameter is interned like any type; there is no unification, so nothing *solves* one side from the other |
| `where Self: B` on a trait *method* | rejected — a supertrait is a property of the trait, so `trait A: B` is the one spelling for it |
| an impl of a supertrait *inferred* from the sub's | none: `impl DoubleEnded for X` requires an `impl Iterator for X` written out. The obligation is checked, never discharged for you |
| an array or a range that *is* an `Iterator` | neither implements the trait; `xs.iter()` / `(0..n).iter()` are the seeds, and `for x in xs` keeps its own desugaring rather than routing through them |
| an array iterator detached from the array it walks | `xs.iter()` holds the array and snapshots nothing, so a `push`/`pop` mid-walk is visible (never out of bounds — see "Sources"). `xs.iter().collect()` is the copy |
| a heterogeneous operator (`V2 * Float`, `Matrix * Vec`) | the `std::ops` traits are homogeneous (`other: Self`, no `Output`): an operator would have to select an impl by its *right* operand, which only the written `Meters::from(x)` / `Into::<U>::into(x)` forms can do. A mixed pair is an argument mismatch on the rewritten call |
| custom `==` (an `Eq` trait) | equality stays structural and import-less for every type; only ordering and arithmetic are traits. `std::map` was the consumer this was waiting on and did not need it: a hash map resolves collisions with the structural `==` on a `K` bounded only by `Hash` |
| bitwise operators (`&`, `\|`, `^`, `<<`, `>>`) | there are none, so anything defined in terms of bits cannot be written in ducktape — `std::hash` reaches C for its mixing, and `std::map` indexes with `%` rather than a mask. Divide by a power of two for a right shift |
| checked or saturating integer arithmetic | `Int` wraps silently, two's complement; `%` keeps the sign of its left operand, so `(0 - 17) % 5` is `-2` |
| hashing a `Float`, and so a `Float` map key | no `impl Hash for Float`: `NaN != NaN` makes such a key unfindable in a table that compares with `==`, and the only Int a Float reaches is a truncating `as`. Wrap one in a struct and say what equality means |
| an `entry`-style API on a map (`or_insert`, in-place update) | `get` then `insert`, which probes twice. Nothing can hold a slot open between the two |
| `Display` for a `HashMap` / `HashSet` | none ships; iteration order is unspecified, so a rendering would have to sort and the key would need `Ord` on top of `Hash`. `m.keys()` and `m.values()` are the arrays |
| a generic unit variant solved by a *call argument's* hint (`slots.push(Slot::Empty)`) | "expected `Slot<K, V>` but got `Slot<_, _>`" — the hint solves the variant's own parameters as far as unknowns but will not then bind them to the enclosing function's type parameters. An annotated local (`var e: Slot<K, V> = Slot::Empty;`) drives it from the other side and does check |
| `a.b < c > (d)` read as a comparison | it is read as a type application. A `<` after a field or method access is disambiguated by looking ahead for the matching `>` and asking whether a `(` follows, since type arguments are only legal immediately before their call; this is the one shape that satisfies that test without meaning it. Parenthesise the left comparison |
| an impl overriding a defaulted method restating its `where` | conformance compares signatures, which carry no bounds, so an override may quietly add or drop one; the trait's own clause is still discharged at every call through the trait |
| `mod` declarations | no such keyword; `use` is what pulls a file in |
| glob imports (`use a::*`) | not parsed; name each item, or bind the module (`use a;`) and qualify |
| re-exporting a module qualifier (`pub use a;`) | a qualifier is not an item; `pub use` re-exports named items only |
| `pub` on a method | rejected — `pub fun` in an impl is "expected impl item", and `pub` is only ignored on the `impl` keyword itself. A method is as visible as the impl it sits in, which is why `std::array`'s raw `pop_last` and `std::iter`'s `char_at` are private *free functions* rather than methods. Struct **fields** do take `pub`: they are private to their module by default, and the diagnostic naming one says so |
| a diagnostic when two *inherent* impls give one type the same method name | there is none: the std one silently wins and the program's is unreachable, so `impl<T> [T] { fun first(..) }` in a program is quietly ignored (`first` reaches every program through the prelude's `std::iter → std::array` chain). A trait impl collision *is* caught — this is only the inherent case, and it is why an opt-in std module leaves a name alone |
| overlap rules finer than "same trait, matching self types" | there is no orphan rule and no specialization: an impl may be written for any type, and two overlapping ones are simply refused wherever both are visible |
| visibility below module granularity (`pub(crate)` &c.) | `pub` is the only modifier |
| two spellings of one file (symlinks, unusual paths) | dedup is lexical, so the file would load twice and collide |
| top-level `var` (globals) | parses, then a registration diagnostic: move it into a function |
| tuple-struct struct-patterns `Pair { a, b }` | write the constructor spelling `Pair(a, b)` — "matching tuple struct with struct pattern syntax is not allowed" |
| variable shadowing diagnostics | a `var` may silently shadow an earlier one in the same scope (top-level *item* names do collide — that is an error) |
| declaring a type whose name is a builtin (`struct Range`, `struct String`) | accepted, but a builtin name is resolved before the type scope is consulted, so every mention of it means the builtin and the declaration is unreachable |
| overlapping method names across impls of one type | bare paths pick the first registered impl (inherent impls do not conflict; only trait impls are checked for overlap) |
| capturing a `for` loop variable in a closure | runs, but the closure sees the loop variable's *final* value (one shared cell), not a per-iteration copy — `runtime.md` "Closures & upvalues" |
| infinitely deep generic instantiation | `fun grow<T>(v: T) { grow([v]) }` type-checks but names a new instantiation at every level; codegen stops at 32 and reports it (`runtime.md` "Monomorphisation") |
| more than 65536 functions, counting one per instantiation | each instantiation takes a global slot, so a heavily generic program can outgrow the two-byte operand space (`runtime.md` "Bytecode") |
| an `@intrinsic` named as a value (`var f = len::<Int>;`) | an intrinsic is an opcode, so there is no body for a global slot to address — "is an intrinsic and can only be called directly" (an `@native` *can* be a value) |
| a generic function named as a value (`var p = print;`) | its type arguments have nothing to solve them — "cannot infer type for 'T'"; call it, or use a non-generic one |
| `@native` on a *trait-declaration* method | rejected: its default body is generic over `Self`, so there is no concrete C body to bind. An *impl* method may be native (milestone 39) |
| generic `main` | nothing calls the entry point, so no instance is ever made — "'main' must not be generic" |
