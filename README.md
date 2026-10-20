# Yuseri

**Yuseri** (ゆせり) is a statically typed programming language for backend services, designed so that an AI can write correct code from the bundled skill alone, without prior training on the language.

- Status: early research. Stage 0 compiler reaches the MVP: a JSON API server written from the skill alone compiles to a native binary and serves HTTP (`std.json`, `std.http`)
- Skill (the language reference): [`skills/SKILL.md`](skills/SKILL.md), [`skills/core.md`](skills/core.md), [`skills/errors.md`](skills/errors.md)
- Announced: 2026-10-20

## Build

Requires clang (or any C11 compiler) and make. No other dependencies.

```
make
make test
```

`build/yuseri fmt <file.yu>` prints a file in canonical layout. `build/yuseri check <file.yu>` type-checks the file and prints every error as one line, `<ID> <file>:<line> <message>; <hint>`, sorted by line; each ID has an entry in `skills/errors.md`.

`build/yuseri test <file.yu>` compiles the file to C (in `.yuseri/`), builds it with the C compiler and runs its `test` blocks; `build/yuseri run <file.yu>` runs `main`. Runtime errors use the same one-line format with `R-` ids.

`tests/run/eval_api.yu` is a JSON API server written by an AI from the skill; `yuseri run` serves it on port 8080, and `sh tests/smoke_http.sh` talks to it over HTTP.

## License

Copyright 2026 kaz-i. Licensed under the Apache License, Version 2.0; see [`LICENSE`](LICENSE).

## Layout

```
skills/      language reference for AI (the source of truth)
stage0/src/  Stage 0 compiler (C, throwaway; compiles Yuseri Core to C)
stage0/rt/   runtime for generated C
std/         standard library signatures
tests/       golden tests
```

---

# ゆせり

**ゆせり（Yuseri）** は、バックエンドサービス向けの静的型付けプログラミング言語です。AI が事前学習なしに、同梱のスキル（言語リファレンス）だけを読んで正しいコードを書けることを目標に設計しています。

- 状態: 研究の初期段階。Stage 0 コンパイラが MVP に到達（スキルだけで AI が書いた JSON API サーバーがネイティブバイナリになり HTTP を受ける。`std.json`、`std.http`）
- スキル（言語リファレンス）: `skills/` 配下
- 命名日: 2026-10-20
- ライセンス: Apache License 2.0（`LICENSE`）
