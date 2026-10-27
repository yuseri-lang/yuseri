#!/bin/sh
# Program tests: `yuseri test tests/run/<name>.yu` must print exactly <name>.expected
# (stdout and stderr merged) and exit with the status recorded in <name>.status (default 0).
set -u
bin=build/yuseri
fail=0
export YUSERI_RT=stage0/rt
for f in tests/run/*.yu; do
  exp="${f%.yu}.expected"
  want=0
  if [ -f "${f%.yu}.status" ]; then want=$(cat "${f%.yu}.status"); fi
  out=$("$bin" test "$f" 2>&1)
  rc=$?
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
  if [ "$rc" != "$want" ]; then
    echo "FAIL    $f (exit $rc, want $want)"
    fail=1
    continue
  fi
  # the formatter must be idempotent on real programs: a formatted copy passes `fmt --check` silently
  tmp=$(mktemp -t yuseri).yu
  cp "$f" "$tmp"
  "$bin" fmt --write "$tmp"
  chk=$("$bin" fmt --check "$tmp" 2>&1)
  rm -f "$tmp"
  if [ -n "$chk" ]; then
    echo "FAIL    $f (fmt --check after fmt --write): $chk"
    fail=1
    continue
  fi
  echo "ok      $f"
done
exit $fail
