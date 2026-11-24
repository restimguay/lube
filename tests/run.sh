#!/bin/sh
# Runs every tests/*.lube program (and tests/text/*.lube: strs of every
# length, each form of them meeting the other) and compares its output (stdout
# and stderr together) with the matching .out file; then the command-line
# tests (tests/cli.sh) with tests/cli.out, and their part that differs from
# system to system with tests/cli.<system>.out (Darwin, Linux or Windows).
# The output of a failing test is kept in build/failed/.
#
#   sh tests/run.sh [path-to-lube]
cd "$(dirname "$0")/.." || exit 1
LUBE=${1:-./lube}
pass=0
fail=0
rm -rf build/failed
# compare NAME EXPECTED-FILE ACTUAL-OUTPUT
compare() {
    if [ "$3" = "$(cat "$2")" ]; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1))
        echo "FAIL $1"
        printf '%s\n' "$3" | diff "$2" - | head -20
        mkdir -p build/failed
        printf '%s\n' "$3" >"build/failed/$(basename "$2")"
    fi
}
for test in tests/*.lube tests/text/*.lube; do
    expected="${test%.lube}.out"
    [ -f "$expected" ] || continue
    compare "$test" "$expected" "$("$LUBE" "$test" 2>&1)"
done
compare tests/cli.sh tests/cli.out "$(sh tests/cli.sh "$LUBE" 2>&1)"
os=$(uname -s)
case "$os" in MINGW* | MSYS* | CYGWIN*) os=Windows ;; esac
if [ -f "tests/cli.$os.out" ]; then
    compare "tests/cli.sh (os)" "tests/cli.$os.out" "$(sh tests/cli.sh "$LUBE" os 2>&1)"
fi
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
