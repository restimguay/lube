#!/bin/sh
# Runs every tests/*.lube program and compares its output (stdout and
# stderr together) with the matching tests/*.out file.
#
#   sh tests/run.sh [path-to-lube]
cd "$(dirname "$0")/.." || exit 1
LUBE=${1:-./lube}
pass=0
fail=0
for test in tests/*.lube; do
    expected="${test%.lube}.out"
    [ -f "$expected" ] || continue
    actual=$("$LUBE" "$test" 2>&1)
    if [ "$actual" = "$(cat "$expected")" ]; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1))
        echo "FAIL $test"
        printf '%s\n' "$actual" | diff "$expected" - | head -20
    fi
done
# command-line tests: tests/cli.sh output against tests/cli.out
actual=$(sh tests/cli.sh "$LUBE" 2>&1)
if [ "$actual" = "$(cat tests/cli.out)" ]; then
    pass=$((pass + 1))
else
    fail=$((fail + 1))
    echo "FAIL tests/cli.sh"
    printf '%s\n' "$actual" | diff tests/cli.out - | head -20
fi
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
