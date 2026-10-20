# std: String methods

Methods on `String`. Always available; no import. Strings are immutable, so every method returns a new value. Indices are in bytes (UTF-8); slicing inside a character is `R-STR-01`.

| Method | Type | Note |
|---|---|---|
| `len()` | `-> Int` | byte length |
| `is_empty()` | `-> Bool` | |
| `split(sep: String)` | `-> List<String>` | empty pieces are kept: `"a=".split("=")` is `["a", ""]`, `"".split("&")` is `[""]` |
| `trim()` | `-> String` | strips ASCII whitespace at both ends |
| `starts_with(p: String)` | `-> Bool` | |
| `ends_with(p: String)` | `-> Bool` | |
| `contains(p: String)` | `-> Bool` | |
| `find(p: String)` | `-> Option<Int>` | byte index of first match |
| `replace(from: String, to: String)` | `-> String` | all occurrences |
| `to_upper()` / `to_lower()` | `-> String` | ASCII only |
| `slice(start: Int, end: Int)` | `-> String` | `end` exclusive |
| `chars()` | `-> List<String>` | one Unicode character each |
| `bytes()` | `-> Bytes` | |
| `join(parts: List<String>)` | `-> String` | `", ".join(xs)`; an empty list gives `""` |
| `repeat(n: Int)` | `-> String` | |
