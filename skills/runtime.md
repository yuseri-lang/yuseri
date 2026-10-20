# Yuseri runtime (layer 2)

Read after `core.md`. Three rules:

1. Values die with the request. State that outlives a request goes in a `Store<T>`.
2. The outside world comes from `System`. `main(sys: System)` hands parts of it (`sys.net`, `sys.clock`, ...) to the functions that need them. A function without such a parameter cannot touch that resource.
3. You do not write concurrency. Requests run in parallel (one worker per CPU); the only thing you start by hand is `sys.tasks.spawn`.

## 1. Lifetimes
Every value made while handling a request lives in that request's arena and is freed with the response; nothing is freed by hand. Values `main` builds before `http.serve` live as long as the process and may be captured by the handler. Shared mutable state goes in a `Store`. Each `test` block has its own arena.

## 2. Store
`Store<T>` holds any Core value with no function, `Store` or capability inside (`E-STORE-01`).
```
let users = Store.new(Map<Int, User>())   // Store<Map<Int, User>>
let snapshot = users.get()               // T: a copy
let after = users.update(fn(m: Map<Int, User>) -> Map<Int, User> { ... })  // T: atomic replace, returns the new value
users.set(Map<Int, User>())              // Unit
let r = stock.try_update(fn(n: Int) -> Result<Int, StockError> {
  if n < 1 { Err(StockError.Insufficient) } else { Ok(n - 1) }
})                                       // Result<T, E>: Err leaves the value unchanged
```
Other requests see the old or the new value, never a mix. A change with a condition is `try_update`: `get` then `update` is not atomic. If `f` panics the value stays and the request fails (`R-STORE-01`); `f` must not touch the same Store (`R-STORE-02`). `update` copies the whole value, so keep stores small. Stores and capabilities are plain parameter types: `fn adjust(stock: Store<Int>, log: Log) -> Response`.

## 3. System and capabilities
```
struct System { net: Net, fs: Fs, env: Env, clock: Clock, random: Random, log: Log, tasks: Tasks }
fn main(sys: System) -> Unit {
  let hits = Store.new(0)
  http.serve(sys.net, 8080, fn(req: Request) -> Response { handle(req, hits, sys.clock) })
}
```
Only `main` receives `System`; a capability cannot be stored in a `Store` or compared (`E-CAP-01`). So a signature says what a function can reach: `fn validate(u: User) -> Result<User, E>` cannot open a socket or read a file. Capabilities are used through methods, except `Net`, which `http.serve` takes.

| Capability | Use | In tests |
|---|---|---|
| `Net` | `http.serve(net, port, handler)`; `http.serve_with(net, Serve { port: 8080, deadline_ms: 5000, workers: None }, handler)` sets the deadline and workers (`None`: one per CPU) | call the handler with `http.request(...)` |
| `Clock` | `clock.now() -> Int` (Unix milliseconds) | `Clock.fixed(1000) -> Clock` |
| `Env` | `env.get("PORT") -> Option<String>` | `Env.of(m: Map<String, String>) -> Env` |
| `Log` | `log.info(msg)`, `log.error(msg) -> Unit`. A line is `<request id> <level> <msg>`: `req-7 info GET /x -> 200`; in a test the id is the test name, spaces included | `Log.capture() -> Log`, then `log.lines() -> List<String>` |
| `Fs` | `fs.read(path) -> Result<String, FsError>`, `fs.write(path, text) -> Result<Unit, FsError>`; `enum FsError { NotFound(path: String), Io(path: String, message: String) }` | `Fs.memory() -> Fs`, an empty in-memory disk |
| `Random` | `random.int(max) -> Int`, uniform, `0 <= n < max` (`R-RAND-01` if `max <= 0`); each call advances the handle | `Random.seeded(42) -> Random`, the same sequence every run |
| `Tasks` | `tasks.spawn(f: fn() -> Unit) -> Unit` (section 4) | `Tasks.inline() -> Tasks` runs `f` at once |

## 4. Tasks and deadlines
`sys.tasks.spawn(fn() -> Unit { notify(mailer, order) })` runs `f` after the response is sent, with the same request id and deadline. `f` returns nothing; write results to a `Store`. A failure stops only that task (`R-TASK-01`). At most 64 tasks per request (`R-TASK-02`).

Every request has a deadline: 30 seconds, or `deadline_ms` of `serve_with`. Past it, the next allocation, loop iteration or I/O fails with `R-TIME-01` and the client gets `500`. Under `http.serve` every stderr line, log or error, starts with the request id.
