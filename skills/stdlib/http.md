# std.http

`import std.http`. A minimal HTTP/1.1 server: one request at a time, one handler function.

## Types

```
struct Request { id: String, method: String, path: String, query: Map<String, String>, headers: Map<String, String>, body: String }
struct Response { status: Int, headers: Map<String, String>, body: String }
```
`id` is the request id; it is also the first entry of every error trace created while handling the request. `method` is upper-case (`GET`). `path` has no query string and is percent-decoded; a trailing `/` is kept as written. `query` holds the decoded `?a=1&b=2` pairs. Header names are lower-case.

## Functions

- `http.serve(port: Int, handler: fn(Request) -> Response) -> Unit` listens on `port` and calls `handler` for every request. It never returns. A `panic` inside the handler answers `500` and the server keeps running. Malformed or incomplete requests are answered with `400`, headers over 64 KiB or bodies over 1 MiB with `413`, without reaching the handler.
- `http.response(status: Int, body: String) -> Response` with `content-type: text/plain; charset=utf-8`.
- `http.json(status: Int, body: String) -> Response` with `content-type: application/json`.
- `http.request(method: String, path: String, body: String) -> Request` builds a request for tests; `path` may carry a query string. `id` is `test`.

## Routing

There is no router. Match on `method` and `path` yourself. For a parameter, strip the prefix and parse the rest:
```
if req.path.starts_with("/users/") {
  let rest = req.path.slice(7, req.path.len())
  match Int.parse(rest) {
    Ok(id) if id > 0 => show_user(id)
    _ => http.json(404, "\{\"error\":\"not found\"\}")
  }
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

fn main() -> Unit { http.serve(8080, handle) }

test "routes" {
  assert_eq(handle(http.request("GET", "/health", "")).status, 200)
  let r = handle(http.request("POST", "/echo", "\{\"message\":\"hi\"\}"))
  assert_eq(r.body, "\{\"message\":\"hi\"\}")
  assert_eq(handle(http.request("GET", "/nope", "")).status, 404)
}
```
