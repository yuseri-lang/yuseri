#!/bin/sh
# Error-output tests: `yuseri check tests/errors/<name>.yu` must print exactly
# <name>.expected on stderr (one error per line, sorted by line).
set -u
bin=build/yuseri
fail=0
for f in tests/errors/*.yu; do
  exp="${f%.yu}.expected"
  out=$("$bin" check "$f" 2>&1 >/dev/null)
  if [ ! -f "$exp" ]; then
    echo "MISSING $exp"
    fail=1
    continue
  fi
  if [ "$out" != "$(cat "$exp")" ]; then
    echo "FAIL    $f"
    printf '%s\n' "$out" | diff "$exp" - | head -30
    fail=1
    continue
  fi
  echo "ok      $f"
done
exit $fail
