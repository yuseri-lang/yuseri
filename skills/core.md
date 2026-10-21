# Yuseri Core

Yuseri is a statically typed language for backend services. This file is the complete Core spec. If something is not here, do not write it. Layout rules are in `SKILL.md`.

## 1. Program
One file = one module. Top-level declarations may appear in any order and refer to each other. An executable module has `fn main() -> Unit`, or `fn main(sys: System) -> Unit` when it touches the outside world (runtime.md); library and test modules have no `main`. Comments start with `//`. Keywords: `fn let var const if else for in while break continue return struct enum impl import pub match test true false`; anything else is free for names. A statement ends at the end of its line unless the line ends with `(`, `[`, `{`, `,`, a binary operator, or `=>`. The prelude (`Option`, `Result`, `List`, `Map`, `assert`, `panic`) needs no import.

## 2. Modules
`import std.json` brings module `json` into scope; call `json.parse(s)`. Only `pub` items (`pub fn`, `pub struct`, ...) are visible outside their module.

## 3. Bindings
`let` is immutable, `var` is mutable, `const` needs a type and a literal. `let` and `var` take no type annotation. An unused binding, parameter, or pattern name is compile error `E-BIND-01`; write `_` for values you drop.
```
let a = 1
var b = 2
b = b + a
const MAX: Int = 100
```
Inference is local: a binding takes its initializer's type. `None`, `Some`, `Ok`, `Err`, `[]`, `List<T>()` take their type from where they are used (return type, parameter, or the other argument of the same call).

## 4. Basic types
`Int` (64-bit), `Float`, `Bool` (`true` / `false`), `String`, `Bytes`, `Unit` (its only value is written `Unit`, as in `Ok(Unit)`). Conversions are explicit functions and never silent:
```
let n = Int.parse("42")       // Result<Int, ParseError>; ParseError has one variant, Invalid
```
`Int.parse` accepts an optional `-` and decimal digits that fit in 64 bits, nothing else. `Float.from(i)` and `String.from(x)` (x: Int, Float or Bool) are the other conversions.

## 5. Expressions
Arithmetic `+ - * / %` on `Int` or `Float` only (no `+` on `String`; use interpolation or `join`); unary `-` (`-1`). Comparison `== != < <= > >=`. Logic `&& || !` (`&&` and `||` short-circuit). Precedence, high to low: unary, `* / %`, `+ -`, comparison, `&&`, `||`. Operands and call arguments are evaluated left to right. `==` compares any Core value structurally; `Map` equality ignores order. Overflow and division by zero fail with `R-INT-01` / `R-INT-02`, never wrap. Any expression can stand as a statement; its value is dropped, except a `Result` (see 13).

## 6. Strings
Immutable UTF-8. `"..."` literals, `{expr}` interpolation, `\"` `\\` `\n` `\{` `\}` escapes; a bare `{` or `}` outside interpolation is `E-STR-01`. Interpolation accepts any Core value (the one implicit conversion) and prints it as source: `Status.Active`, `[1, 2]`, `Some(3)`.
```
let msg = "user {u.id}: {u.name}"
```
Methods (`len`, `split`, `trim`, ...) are listed in `stdlib/string.md`.

## 7. Functions
```
fn add(a: Int, b: Int) -> Int { a + b }
```
Parameter and return types are required. The last expression is the return value; `return x` exits from any depth. A block whose last line is a statement has type `Unit`.

## 8. Function values
A function type is `fn(Int) -> Int`. A named function is a value of its type; a function value is called like any function. An anonymous function is `fn` without a name, same rules; it captures `let` bindings by value and cannot capture `var` (blocks and loops are not functions and use enclosing `var`s freely).
```
let handlers = Map<String, fn(Request) -> Response>()
handlers.set("GET /health", health)
let r = handlers.get(key).unwrap_or(not_found)(req)
let doubled = items.map(fn(x: Int) -> Int { x * 2 })
```

## 9. Control flow
`if` with `else` is an expression; both branches have the same type. `if` without `else` is a statement of type `Unit`. `else if` chains. `for` iterates a range or any `List` expression; a `Map` via `entries()`. `while` loops on a condition. `break` and `continue` work in loops.
```
let size = if n > 10 { "big" } else if n > 5 { "mid" } else { "small" }
if n < 0 { return None }
for i in 0..n { }          // 0 to n-1
for e in m.entries() { }   // e.key, e.value
```

## 10. Structs
```
struct User { id: Int, name: String }
let u = User { id: 1, name: "Ann" }
let v = User { ..u, name: "Bo" }   // copy with changes
```
Structs are values. Fields are comma-separated; a trailing comma is allowed. Fields of a `var` struct can be assigned: `w.name = "Cy"`.

## 11. Enums
```
enum Status { Active, Banned(reason: String) }
let s = Status.Banned(reason: "spam")
```
Variants may carry named fields. A variant without fields is written `Status.Active`, in values and in patterns. Separators follow the struct rule.

## 12. Option
`Option<T>` is `Some(value)` or `None`. Use `match` or `?` (see 13) to get the value. Methods: `is_some()`, `unwrap_or(default)`, `ok_or(err)` (to `Result`).
```
let u = find(1).unwrap_or(guest)   // find returns Option<User>
```

## 13. Result and `?`
`Result<T, E>` is `Ok(value)` or `Err(error)`. `E` is an enum you define. `?` returns the `Err` from the current function, whose return type must be `Result` with the same `E`. `?` on `Option` returns `None`.
```
enum AppError { NotFound(id: Int), Invalid(field: String) }

fn load(id: Int) -> Result<User, AppError> {
  let u = find(id).ok_or(AppError.NotFound(id: id))?
  Ok(u)
}
```
Methods: `is_ok()`, `unwrap_or(default)`. A value used as `E` carries a hidden trace: `Err(e)` and each `?` add a `file:line`. `e.trace()` returns `List<String>`: the request id, then the hops; empty if never wrapped. A `Result` that is not bound, passed, matched, or `?`-ed is compile error `E-RES-01`.

## 14. Pattern matching
`match` must cover every case (`E-MATCH-01` otherwise). Arms are `pattern => expr` or `pattern => { block }` and all arms have the same type. `_` matches anything. `if` adds a guard.
```
match load(id) {
  Ok(u) if u.id > 0 => u.name
  Ok(_) => "guest"
  Err(AppError.NotFound(id: id)) => "no user {id}"
  Err(e) => "error: {e}"
}
```

## 15. List
`List<T>`, ordered, growable on `var`. `List` and `Map` are values: assigning copies. `xs[i]` returns the element and fails with `R-LIST-01` out of range; `get(i)` returns `Option<T>` instead.
```
var xs = List<Int>()
xs.push(3)               // Unit; needs var
let first = xs[0]
let maybe = xs.get(9)     // None
let ys = [1, 2, 3]
```
Other methods: `len()`, `map(f)`, `filter(f)`, `contains(x)`.

## 16. Map
`Map<K, V>`, insertion-ordered. `set` returns `Unit` and replaces an existing key's value in place. `entries()` yields values with `key` and `value` fields.
```
var m = Map<String, Int>()
m.set("a", 1)
let v = m.get("a")        // Option<Int>
let has = m.has("a")
```
Other methods: `len()`, `remove(k) -> Unit`.

## 17. Methods
`impl Type { ... }` attaches functions. `self` is the receiver and is read-only; return a new value instead of mutating.
```
impl User {
  fn display(self) -> String { "{self.id}:{self.name}" }
}
let text = u.display()
```

## 18. Generics
Type parameters go in `<...>` on functions and types. Call sites never spell them out.
```
fn first<T>(xs: List<T>) -> Option<T> { xs.get(0) }
```

## 19. Tests
`test` blocks sit next to the code; `yuseri test` runs them. `assert(cond: Bool) -> Unit`, `assert_eq<T>(a: T, b: T) -> Unit`; a failed assert ends that test. A test is not a function: no `?` inside, `match` the `Result` instead.
```
test "add works" {
  assert_eq(add(2, 3), 5)
}
```

## 20. Panic
`panic("msg")` stops the request with `R-PANIC-01` and fits any type position. Use it only for impossible states.

## 21. Errors and this spec
Compile errors are `E-<AREA>-<NN>`, runtime errors `R-<AREA>-<NN>`. Each error prints one line: id, file:line, message, and a fix hint. The id maps to a section of `errors.md`.
```
E-RES-01 app/users.yu:12 unused Result from load(); add `?` or match it
```
