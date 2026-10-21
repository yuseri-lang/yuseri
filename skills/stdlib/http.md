# std.http

`import std.http`. A minimal HTTP/1.1 server: one worker per CPU, each handling one request at a time; one handler function.

## Types

```
struct Request { id: String, method: String, path: String, query: Map<String, String>, headers: Map<String, String>, body: String }
struct Response { status: Int, headers: Map<String, String>, body: String }
struct Serve { port: Int, deadline_ms: Int, workers: Option<Int> }
```
After `import std.http` these types are used by their plain names (`Request`, not `http.Request`); the functions keep the `http.` prefix. `id` is the request id; it is also the first entry of every error trace created while handling the request. `method` is upper-case (`GET`). `path` has no query string and is percent-decoded; a trailing `/` is kept as written. `query` holds the decoded `?a=1&b=2` pairs. Header names are lower-case.

## Functions

- `http.serve(net: Net, port: Int, handler: fn(Request) -> Response) -> Unit` needs the `Net` capability from `main(sys: System)` (runtime.md 3) and listens on `port` and calls `handler` for every request. It never returns. A `panic` inside the handler, or a request past its deadline (`R-TIME-01`, 30 seconds), answers `500` and the server keeps running. Malformed or incomplete requests are answered with `400`, headers over 64 KiB or bodies over 1 MiB with `413`, without reaching the handler.
- `http.serve_with(net: Net, opts: Serve, handler: fn(Request) -> Response) -> Unit` is `serve` with the deadline in milliseconds and the worker count (`None`: one per CPU): `http.serve_with(sys.net, Serve { port: 8080, deadline_ms: 5000, workers: None }, handle)`.
- `http.response(status: Int, body: String) -> Response` with `content-type: text/plain; charset=utf-8`.
- `http.json(status: Int, body: String) -> Response` with `content-type: application/json`.
- `http.request(method: String, path: String, body: String) -> Request` builds a request for tests; `path` may carry a query string. `id` is the test name, the same id that log lines and traces carry in that test.

## Routing

There is no router. Match on `method` and `path` yourself: one `if` per route that `return`s, the `404` last. For a parameter, strip the prefix (and the suffix) with `slice` and parse the rest:
```
fn route(req: Request) -> Response {
  let m = req.method
  let p = req.path
  if m == "GET" && p.starts_with("/users/") {
    return match Int.parse(p.slice(7, p.len())) {
      Ok(id) if id > 0 => show_user(id)
      _ => http.json(404, "\{\"error\":\"not found\"\}")
    }
  }
  if m == "POST" && p.starts_with("/orders/") && p.ends_with("/cancel") {
    return cancel_order(p.slice(8, p.len() - 7))  // "/orders/12/cancel" -> "12"
  }
  http.response(404, "not found")
}
```

## Example

```
import std.http
import std.json

struct Echo { message: String }

fn handle(req: Request) -> Response {
  match "{req.method} {req.path}" {
    "GET /health" => http.response(200, "ok")
    "POST /echo" => match json.decode<Echo>(req.body) {
      Ok(e) => http.json(200, json.encode(e))
      Err(err) => http.json(400, "\{\"error\":\"{err}\"\}")
    }
    _ => http.response(404, "not found")
  }
}

fn main(sys: System) -> Unit { http.serve(sys.net, 8080, handle) }

test "routes" {
  assert_eq(handle(http.request("GET", "/health", "")).status, 200)
  let r = handle(http.request("POST", "/echo", "\{\"message\":\"hi\"\}"))
  assert_eq(r.body, "\{\"message\":\"hi\"\}")
  assert_eq(handle(http.request("GET", "/nope", "")).status, 404)
}
```
