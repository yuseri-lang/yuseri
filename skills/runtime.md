# Yuseri runtime (layer 2)

Read after `core.md`. Three rules cover everything here:

1. Values die with the request. State that must outlive a request goes in a `Store<T>`.
2. The outside world comes from `System`. `main(sys: System)` receives it and hands parts of it (`sys.net`, `sys.clock`, ...) to the functions that need them. A function without such a parameter cannot touch that resource.
3. You do not write concurrency. The runtime runs requests in parallel; the only thing you start by hand is `sys.tasks.spawn`.

## 1. Lifetimes
Every value made while handling a request lives in that request's arena and is freed when the response is sent. Nothing is freed by hand and nothing leaks across requests. Values `main` builds before `http.serve` live as long as the process; they are `let` values, so capturing them in the handler is safe. Mutable state that requests share goes in a `Store`. Each `test` block has its own arena.

## 2. Store
`Store<T>` is the one place that outlives requests. `T` is any Core value (struct, List, Map, Int, String, Option, enum) that holds no function, no `Store` and no capability (`E-STORE-01` otherwise).
```
let users = Store.new(Map<Int, User>())            // Store<Map<Int, User>>
let snapshot = users.get()                        // T: a copy, yours for this request
let after = users.update(fn(m: Map<Int, User>) -> Map<Int, User> { ... })  // T: atomic replace, returns the new value
users.set(Map<Int, User>())                       // Unit: update with a constant
let r = stock.try_update(fn(n: Int) -> Result<Int, StockError> {
  if n < 1 { Err(StockError.Insufficient) } else { Ok(n - 1) }
})                                                // Result<T, E>: Err leaves the value unchanged
```
`get` copies; changing the copy changes nothing. `update` applies `f` to the current value, replaces it and returns the new value; other requests see either the old or the new value, never a mix. Use `try_update` when the change has a condition: checking with `get` and then calling `update` is not atomic across requests. If `f` panics, the value stays; the panic line is followed by `R-STORE-01` and the request fails as any panic does. `update` copies the whole value, so keep stores small; large data belongs in a database.

Capabilities and stores are plain parameter types: `fn adjust(stock: Store<Map<String, Int>>, clock: Clock, log: Log) -> Response`.

## 3. System and capabilities
```
struct System { net: Net, fs: Fs, env: Env, clock: Clock, random: Random, log: Log, tasks: Tasks }
fn main(sys: System) -> Unit {
  let hits = Store.new(0)
  http.serve(sys.net, 8080, fn(req: Request) -> Response { handle(req, hits, sys.clock) })
}
```
Only `main` receives `System`. Every other function takes the capabilities it needs as parameters; there is no global way to get one. Capabilities cannot be stored in a `Store` or compared (`E-CAP-01`); they travel only through parameters and captures. So a signature tells you what a function can reach: `fn validate(u: User) -> Result<User, E>` cannot open a socket or read a file. Each capability is used through its methods (`clock.now()`), except `Net`, which `http.serve` takes as an argument.

| Capability | Use | In tests |
|---|---|---|
| `Net` | `http.serve(net, port, handler)` | call the handler with `http.request(...)`; no `Net` needed |
| `Clock` | `clock.now() -> Int` (Unix milliseconds) | `Clock.fixed(1700000000000) -> Clock` |
| `Env` | `env.get("PORT") -> Option<String>` | `Env.of(m: Map<String, String>) -> Env` |
| `Log` | `log.info(msg) -> Unit`, `log.error(msg) -> Unit`. Each line is `<request id> <level> <msg>`, e.g. `req-7 info GET /x -> 200`; in a test the request id is the test name | `Log.capture() -> Log`, then `log.lines() -> List<String>` |
| `Fs` | `fs.read(path) -> Result<Bytes, FsError>`, `fs.write(path, bytes)` | `Fs.memory()` |
| `Random` | `random.int(max) -> Int` | `Random.seeded(42)` |
| `Tasks` | `tasks.spawn(f)` (section 4) | `Tasks.inline()` runs `f` immediately |

A program that touches none of these has `fn main() -> Unit` (core.md 1) or no `main` at all.

## 4. Tasks
```
sys.tasks.spawn(fn() -> Unit { notify(mailer, order) })
```
Runs `f` after the response is sent, on its own arena. `f` sees only its captured `let` values (copied), `Store`s, and capabilities passed in. It returns nothing; write results to a `Store`. A failure inside `f` stops only that task and prints one error line with the original request id.

## 5. Deadlines
Every request has a deadline (30 seconds unless `http.serve` is given another). Past it, the next allocation or I/O fails with `R-TIME-01`, the client gets `500`, and the request's arena is freed. Spawned tasks inherit the deadline.

## 6. Errors
`R-STORE-01` update function panicked (value unchanged, request failed). `R-TASK-01` a spawned task failed. `R-TIME-01` deadline passed. `E-STORE-01` type cannot live in a Store. `E-CAP-01` capability stored or copied. All runtime lines carry the request id; `e.trace()` continues across `spawn`.
