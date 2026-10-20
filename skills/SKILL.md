---
name: yuseri
description: Write, test, and fix Yuseri code. Yuseri is a small statically typed language for backend services; this skill is its complete reference.
---

# Yuseri

Yuseri is a statically typed language for backend services, designed to be written correctly on the first try from this reference alone. You have no prior knowledge of it. Everything you need is in these files; if something is not here, it does not exist.

## Files

- `core.md`: the language. Read it fully before writing any code.
- `stdlib/string.md`: String methods. Read when you touch strings.
- `stdlib/<module>.md`: other modules (`http`, `json`, ...). Read only the one you import.
- `errors.md`: one entry per error id. Read the entry named in an error message.

## Rules

1. Braces for blocks, no semicolons, one statement per line.
2. Every function signature is explicit. Inference is local to a function body.
3. No null: use `Option`. No exceptions: use `Result` and `?`.
4. No implicit conversion, no inheritance, no operator overloading, no macros.
5. Bindings are immutable; `var` only when you assign, `push`, or `set`.
6. There is one way to write each thing. Do not invent syntax; if `core.md` lacks it, you do not need it.
7. Put a `test` block next to every function you write and make it pass.
8. Run `yuseri fmt` before showing code; never hand-format.
9. On an error, read the entry for its id in `errors.md`, apply the fix hint, and re-run. Do not guess.
10. Use `panic` only for states that cannot happen. Expected failures are `Err` values.

## Workflow

1. Read `core.md`.
2. Write the code and its `test` blocks.
3. Run `yuseri test`.
4. For each error line (`E-xxx-NN file:line message; hint`), fix as the hint says.
5. Stop when tests pass and `yuseri fmt` reports no change.
