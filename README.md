# Yuseri

**Yuseri** (ゆせり) is a statically typed programming language for backend services, designed so that an AI can write correct code from the bundled skill alone, without prior training on the language.

- Status: early research. Stage 0 compiler in progress (M0 lexer, parser, formatter; M1 type checking; M2 C code generation, runtime and test runner)
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

- 状態: 研究の初期段階。Stage 0 コンパイラを実装中（M0 字句解析・構文解析・フォーマッタ、M1 型検査、M2 C 生成・ランタイム・テスト実行）
- スキル（言語リファレンス）: `skills/` 配下
- 命名日: 2026-10-20
