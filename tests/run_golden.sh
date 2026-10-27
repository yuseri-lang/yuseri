#!/bin/sh
# Golden tests for `yuseri fmt`: tests/golden/<name>.yu must format to <name>.expected,
# and formatting the expected output again must not change it (idempotence).
# `fmt --check` must accept the expected output silently and reject an input that differs
# from it with `<file>:<line> is not formatted` and exit 1.
set -u
bin=build/yuseri
fail=0
for f in tests/golden/*.yu; do
  exp="${f%.yu}.expected"
  out=$("$bin" fmt "$f" 2>&1)
  if [ ! -f "$exp" ]; then
    echo "MISSING $exp"
    fail=1
    continue
  fi
  if [ "$out" != "$(cat "$exp")" ]; then
    echo "FAIL    $f"
    printf '%s\n' "$out" | diff "$exp" - | head -20
    fail=1
    continue
  fi
  tmp=$(mktemp)
  printf '%s\n' "$out" > "$tmp"
  out2=$("$bin" fmt "$tmp" 2>&1)
  if [ "$out" != "$out2" ]; then
    rm -f "$tmp"
    echo "FAIL    $f (not idempotent)"
    fail=1
    continue
  fi
  chk=$("$bin" fmt --check "$tmp" 2>&1)
  rc=$?
  rm -f "$tmp"
  if [ "$rc" != 0 ] || [ -n "$chk" ]; then
    echo "FAIL    $f (fmt --check rejects the canonical layout): $chk"
    fail=1
    continue
  fi
  if [ "$out" != "$(cat "$f")" ]; then
    chk=$("$bin" fmt --check "$f" 2>&1)
    rc=$?
    case "$chk" in
      "$f:"*" is not formatted") ;;
      *) rc=99 ;;
    esac
    if [ "$rc" != 1 ]; then
      echo "FAIL    $f (fmt --check must report the file, exit 1): $chk"
      fail=1
      continue
    fi
  fi
  echo "ok      $f"
done
exit $fail
