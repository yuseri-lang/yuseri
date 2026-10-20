# Yuseri errors

Every error is one line: `<ID> <file>:<line> <message>; <fix hint>`. When a declared type is known, the message names it. Find the ID below, apply the fix, re-run. Compile errors are `E-`, runtime errors `R-`. `<AREA>` names the part of `core.md` to re-read.

## E-SYN: syntax

- **E-SYN-01** unexpected token. A character or word the grammar does not allow here. Fix: compare the line with the example in the named section; remove semicolons, `return` outside a function, or trailing operators.
- **E-SYN-02** unclosed block or literal. A `{`, `(`, `[` or `"` has no partner. Fix: close it on the line the hint names.
- **E-SYN-03** `break` or `continue` outside a loop. Fix: remove it, or move it inside the `for` / `while` it belongs to.

## E-NAME: names and modules

- **E-NAME-01** unknown name. Not defined, not imported, or defined later in a block. Fix: define it, `import` its module, or check the spelling.
- **E-NAME-02** duplicate definition in the same scope. Fix: rename one.
- **E-NAME-03** item is not `pub`. You used something from another module that is private. Fix: add `pub` in its module.

## E-TYPE: types

- **E-TYPE-01** type mismatch: expected X, got Y. Fix: convert explicitly (`Int.parse`, `String.from`, ...) or change the declared type.
- **E-TYPE-02** operator needs same-type `Int` or `Float` operands. Also raised for `+` on `String`. Fix: convert one side, or use interpolation / `join` for strings.
- **E-TYPE-03** missing parameter or return type. Signatures are never inferred. Fix: write `name: Type` for every parameter and `-> Type` after the list.
- **E-TYPE-04** branches have different types (`fn status returns String; else branch is Int`). `if`/`else` or `match` arms must agree. Fix: make every arm the declared type; an `if` used as a statement must not produce a value.
- **E-TYPE-05** cannot infer the type of `None`, `[]`, `List<T>()` or similar. No return type, parameter, or other argument fixes it. Fix: use it where a type is known, or build the value with its type (`List<Int>()`).
- **E-TYPE-06** wrong number of arguments or unknown field. Fix: match the signature or struct definition in the named section.

## E-BIND: bindings

- **E-BIND-01** unused binding, parameter, or pattern name. Fix: delete an unused `let` / `var`; name an unused parameter or pattern field `_`.
- **E-BIND-02** assignment, `push`, or `set` on a `let`. Fix: declare it with `var`.
- **E-BIND-03** anonymous function captures a `var`. Fix: pass the value as a parameter, or copy it into a `let` first.

## E-RES: Result and `?`

- **E-RES-01** unused `Result`. A `Result` value is not bound, passed, matched, or followed by `?`. Fix: add `?`, `match` it, or bind it (`let _ = expr` counts and means "ignored on purpose").
- **E-RES-02** `?` outside a matching function. The function does not return `Result` with this `E` (or `Option` for `?` on `Option`). Fix: change the return type to `Result<T, E>` so callers use `?` too; `match` with a default only when that default is correct.
- **E-RES-03** the error type of Result must be an enum, got `String` (or another non-enum). `E` carries the trace, so it must be an enum you define (core.md 13). Fix: define an enum for the error (`enum AppError { NotFound, Invalid(rule: String) }`) and use it as E; keep the message text in a variant field.

## E-MATCH: pattern matching

- **E-MATCH-01** non-exhaustive `match`; missing: <cases>. Fix: add an arm for each listed case, or a final `_ =>` arm.
- **E-MATCH-02** unreachable arm. An earlier arm already covers it. Fix: remove it or reorder.

## E-STR: strings

- **E-STR-01** bare `{` or `}` in a string literal. Fix: write `\{` or `\}`, or use `{expr}` for interpolation.

## E-FMT: layout

- **E-FMT-01** file is not formatted. Fix: run `yuseri fmt`.

## E-JSON: std.json

- **E-JSON-01** type cannot be converted to or from JSON (an enum variant with fields, a `Map` with non-String keys, a function, or `Bytes`). Fix: use structs, lists, `Map<String, V>`, Int, Float, Bool, String, Option and field-less enums only.

## E-STORE: Store

- **E-STORE-01** type cannot live in a Store (it contains a function, a Store or a capability). Fix: keep only plain data in a Store: structs, lists, maps, Int, Float, Bool, String, Option and enums.

## E-CAP: capabilities

- **E-CAP-01** a capability (`Net`, `Clock`, `Env`, `Log`, `Fs`, `Random`, `Tasks`) is stored in a Store, compared, or converted. Fix: pass capabilities as parameters or capture them; never put them in a Store or compare them.

## R-INT: integers

- **R-INT-01** integer overflow. Fix: check the range before the operation, or use `Float`.
- **R-INT-02** division by zero. Fix: test the divisor first and return `Err`.

## R-LIST, R-STR: collections and strings

- **R-LIST-01** index out of range: index i, len n. Fix: use `get(i)` and handle `None`, or check `len()` first.
- **R-STR-01** slice boundary inside a UTF-8 character. Fix: compute boundaries from `find` or `chars()`, not from arbitrary byte counts.

## R-PANIC

- **R-PANIC-01** panic: <message>. The request stopped at an impossible state. The trace lists the request id and `file:line` hops. Fix: if the state is possible, return `Err` instead of `panic`.

## R-ASSERT: tests

- **R-ASSERT-01** assertion failed: <expression>, or assert_eq failed: left <value>, right <value>. The test stops; other tests still run. Fix: the test's expectation did not hold; fix the code or the test.

## R-HTTP: std.http

- **R-HTTP-01** the server cannot create a socket, bind or listen on the port. Fix: check that the port is free and below 65536.

## R-STORE: Store

- **R-STORE-01** the function passed to `update` panicked (its own error line comes first). The stored value is unchanged and the request fails. Fix: make the update function total, or validate before calling `update`.
- **R-STORE-02** `update`, `set` or `get` was called on a Store from inside that same Store's update function. The value is unchanged and the request fails. Fix: compute the new value from the function's argument only; other Stores may be read.

## R-TIME: deadlines

- **R-TIME-01** the request passed its deadline (30 seconds unless `http.serve_with` sets another). The client gets `500` and the request's memory is freed. Fix: make the handler faster, or give `http.serve_with` a longer deadline.

## R-TASK: spawned tasks

- **R-TASK-01** a task started with `tasks.spawn` failed (its own error line comes first). Only the task stops; the response was already sent. A trace made inside a task starts with the request id, then the `spawn` site. Fix: handle the failing case inside the task, for example by writing an `Err` to a `Store`.
- **R-TASK-02** one request spawned more than 64 tasks. Fix: batch the work into one task.

## R-RAND: Random

- **R-RAND-01** `random.int(max)` was called with `max <= 0`. Fix: check that max is positive before calling `random.int`.

## R-APP: application errors

User code chooses its own `R-APP-NN` ids for `Err` values it logs (for example in a `log_line` function). They are not raised by the runtime. Keep one id per enum variant and list them in your module's README.
