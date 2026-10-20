#!/bin/sh
# Golden tests for `yuseri fmt`: tests/golden/<name>.yu must format to <name>.expected,
# and formatting the expected output again must not change it (idempotence).
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
  rm -f "$tmp"
  if [ "$out" != "$out2" ]; then
    echo "FAIL    $f (not idempotent)"
    fail=1
    continue
  fi
  echo "ok      $f"
done
exit $fail
