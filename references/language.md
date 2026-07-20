# ducktape — language reference (as implemented)

Every example below compiles today; most are lifted from `tests/`. The grammar
in EBNF form is `references/grammar.ebnf`. Anything not listed here is either
in the "Not yet implemented" table at the bottom or does not exist.

Comments run from `#` to end of line. The test harness assigns meaning to two
prefixes: `#! expect:` (first line of a `tests/fail` file) and `#>` (expected
stdout in `tests/run` files) — the language itself treats them as comments.

## Declarations

```
use std::io::print;                  # parsed; imports are a no-op for now

fun add(a: Int, b: Int) -> Int {     # return type via ->, omitted = ()
    a + b                            # last expression is the return value
}

pub struct Point<T> { x: T, y: T, }  # named struct; pub is parsed
struct Pair(Int, Int)                # tuple struct
struct Unit;                         # unit struct

enum Result<T, E> { Ok(T), Err(E), }

trait Drawable {                     # traits register + name-resolve only;
    type Color;                      # items are not checked yet
    fun draw(self) -> Self.Color;
    fun visible(self) -> Bool { return true; }
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
| `Int` `Float` `Bool` `String` | primitives (`String` has no runtime yet) |
| `()` | unit |
| `(A, B)` | tuple |
| `[T]` | array of `T` |
| `fun(A, B) -> R` | function type |
| `Point<Int>` | generic instance |
| `Self`, `Self.Color`, `Point.Color` | self type, associated types |

## Statements and blocks

```
var x = 1;                    # inferred; no `let`, no `mut` — all vars mutable
var y: [Int] = [1, 2, 3];     # annotated
var (a, b) = (1, 2.5);        # tuple destructuring
var Point { x, y } = p;       # struct destructuring (named structs only)
x = 2;  x += 1;               # assignment / compound assignment (+= -= *= /=)
return expr;  return;         # bare return means ()
break;  continue;             # inside loops only
```

Blocks are expressions: the trailing expression without `;` is the block's
value; otherwise the block is `()`. A trailing block-form `if`/`match` counts
as the tail (`fun abs(n: Int) -> Int { if n < 0 { -n } else { n } }`). A block
ending in `return` has type `!` (never), which unifies with anything.

## Expressions

- Arithmetic `+ - * / %` on numerics; `Int op Float` widens to `Float`.
  `+` also concatenates `String`s (checker only, no runtime yet).
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
- Paths: `Result::Ok(5)`, `Point::new(..)`. Explicit type arguments use
  turbofish in expression position: `Point::<Int>::new(1, 2)`. A bare
  `Point::new(..)` on a generic type selects the impl by method name and
  infers the type arguments from the call (first name match wins if several
  impls define the same name).
- Method calls: `p.draw()`, `p.x`, tuple access `t.0`.

### match

```
var m = match p {
    Point { x: 10, y: val } if val > 0 => true,   # guard sees bindings
    _ => false,
};
```

Patterns: literals, `_`, bindings, tuples `(a, b)`, variants
`Result::Ok(v)` / `Status::Active`, structs `Point { x, y }` (including
literal sub-patterns `x: 10`). Exhaustiveness is **not** checked.

### `?` propagation

`expr?` is structural: the operand must be an enum with exactly two
single-field tuple variants named `Ok` and `Err`, and the enclosing function
(or closure) must return the *same* enum; the `Err` payload types must unify.
The result is the `Ok` payload. There is no built-in `Result` — declare your
own (`tests/pass/propagate.dt`). This rule will move to a `Try` trait once
traits are fully checked.

## Built-ins

`print<T>(value)` — prints any value followed by a newline, returns `()`.
Registered in every module; the only builtin so far.

## Not yet implemented

| Gap | Behavior today |
|---|---|
| trait-item checking / conformance | trait bodies ignored; impls unchecked against their trait |
| trait method calls via bounds, default methods | method must come from an impl |
| inline bounds `<T: Display>` | parse + "not supported" error; `where` clauses parse, bounds unenforced |
| modules / imports | `use` is a no-op; single file only |
| top-level `var` | parses, then aborts registration |
| tuple-struct struct-patterns `var Pair { .. }` | diagnostic suggests tuple destructuring |
| assoc types on generic params `T.Item` | "not yet supported" diagnostic |
| match exhaustiveness, variable shadowing diags | silently accepted |
| overlapping method names across impls of one type | bare paths pick the first registered impl |
| `String` runtime, arrays/structs/enums/closures at runtime | compile-only until the GC milestone (`runtime.md`) |
