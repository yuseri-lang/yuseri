#!/bin/sh
# Runtime tests: C programs under tests/rt/ built against stage0/rt with sanitizers.
set -u
fail=0
mkdir -p build/rt
for f in tests/rt/*.c; do
  name=$(basename "$f" .c)
  if ! cc -std=c11 -D_GNU_SOURCE -O1 -g -fsanitize=address,undefined -w -I stage0/rt -I stage0/src -o "build/rt/$name" "$f" stage0/rt/*.c -lm -lpthread; then
    echo "FAIL    $f (build)"
    fail=1
    continue
  fi
  if "build/rt/$name"; then echo "ok      $f"; else echo "FAIL    $f"; fail=1; fi
done
exit $fail
