#!/bin/sh
# The skill (skills/errors.md) is the source of truth for error ids. This checks that
# stage0/src/errors.def lists exactly the same ids.
set -u
a=$(mktemp)
b=$(mktemp)
grep -o '\*\*[ER]-[A-Z]*-[0-9][0-9]\*\*' skills/errors.md | tr -d '*' | sort -u > "$a"
grep -o '"[ER]-[A-Z]*-[0-9][0-9]"' stage0/src/errors.def | tr -d '"' | sort -u > "$b"
if diff "$a" "$b"; then
  echo "ok      error ids match skills/errors.md"
  rc=0
else
  echo "FAIL    error ids differ between skills/errors.md (<) and errors.def (>)"
  rc=1
fi
rm -f "$a" "$b"
exit $rc
