# std.json

`import std.json`. Converts Core values to and from JSON text.

## Encoding

`json.encode(value) -> String` takes any value made of: `struct`, `List<T>`, `Map<String, V>`, `Int`, `Float`, `Bool`, `String`, `Option<T>` (`None` is `null`), and enums whose variants have no fields (the variant name as a string). Struct fields keep their names and order. No spaces are emitted. Strings escape `"`, `\\`, and control characters (`\n`, `\t`, `\u00XX`); other characters, including non-ASCII, are written as they are.
```
struct User { id: Int, name: String, tags: List<String> }
json.encode(User { id: 1, name: "Ann", tags: ["a"] })   // {"id":1,"name":"Ann","tags":["a"]}
```
A value that contains an enum variant with fields, a `Map` with non-String keys, a function, or `Bytes` is compile error `E-JSON-01`.

## Decoding

`json.decode<T>(s: String) -> Result<T, JsonError>` builds a `T` from JSON text. This is the one place a type argument is written at a call: `T` appears in no argument, so you name it.
```
let u = json.decode<User>(body)?
```
Rules: an object fills a struct; every field must be present unless its type is `Option<T>` (missing or `null` gives `None`); unknown keys are ignored; an array fills a `List`; an object fills a `Map<String, V>`; a string fills a field-less enum by variant name; numbers fill `Int` only when they have no fraction or exponent (a minus sign is fine) and fit in 64 bits; any number fills `Float`.
```
enum JsonError { Syntax(at: Int), Shape(path: String, expected: String) }
```
`Syntax` carries the byte offset of the first bad character. `Shape` names the path (`users[2].id`) and the expected type when the text is valid JSON but does not fit `T`.

JSON text in a Yuseri string literal needs `\{` and `\}` (core.md 6); the examples below show it.

## Example

```
import std.json

struct Point { x: Int, y: Int }

fn move_right(body: String) -> Result<String, JsonError> {
  let p = json.decode<Point>(body)?
  Ok(json.encode(Point { ..p, x: p.x + 1 }))
}

test "round trip" {
  assert_eq(move_right("\{\"x\":1,\"y\":2\}"), Ok("\{\"x\":2,\"y\":2\}"))
  assert_eq(move_right("\{\"x\":1\}"), Err(JsonError.Shape(path: "y", expected: "Int")))
  assert_eq(move_right("\{x"), Err(JsonError.Syntax(at: 1)))
}
```
