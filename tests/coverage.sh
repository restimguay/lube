#!/bin/sh
# Measures how much of the interpreter the test suite runs.
#
#   sh tests/coverage.sh        (or: make coverage)
#
# Builds an instrumented interpreter with clang's source-based coverage, runs
# the whole test suite with it, and reports line, function, region and branch
# coverage, listing every line that never ran.
#
# A few lines are defensive checks for situations the rest of the
# interpreter already rules out, so no test can reach them. They are marked
# LCOV_EXCL_LINE (or sit between LCOV_EXCL_START and LCOV_EXCL_STOP); they
# are excluded and their number is reported.
#
# Fails unless every other line and every function ran.
set -e
cd "$(dirname "$0")/.."
OUT=build/coverage
rm -rf "$OUT"
mkdir -p "$OUT"

tool() { command -v "$1" 2>/dev/null || xcrun --find "$1" 2>/dev/null || true; }
PROFDATA=$(tool llvm-profdata)
COV=$(tool llvm-cov)
if [ -z "$PROFDATA" ] || [ -z "$COV" ]; then
    echo "coverage needs clang with llvm-profdata and llvm-cov" >&2
    exit 1
fi

FLAGS=""
if [ "$(uname -s)" = Darwin ]; then FLAGS="-DLUBE_STACK_SIZE=0x3F00000 -Wl,-stack_size,0x3F00000"; fi
# shellcheck disable=SC2086
# atomic counters: isolates run on several threads at once
# LUBE_SILENT: no sound device (the tests play sounds in silence, and make no sound). LUBE_NO_DISPLAY: no
# windows of the system's (the tests' are shown nowhere, or by a host program of their own). The plugins that
# come with Lube are inside it (LUBE_STATIC_PLUGINS), so they are measured too; loading one from a file is
# measured with the tests' own (tests/plugins/).
${CC:-cc} -std=c99 -O0 -g -fprofile-instr-generate -fcoverage-mapping -fprofile-update=atomic -DLUBE_SILENT -DLUBE_NO_DISPLAY -DLUBE_STATIC_PLUGINS $FLAGS -o "$OUT/lube" src/*.c src/plugins/*.c -lm -lpthread

# Programs built with lube --build --native are measured too: they run the
# part of the interpreter that calls machine code.
export LUBE_NATIVE_CFLAGS="-O0 -fprofile-instr-generate -fcoverage-mapping -fprofile-update=atomic -DLUBE_SILENT -DLUBE_NO_DISPLAY"
# The embedding tests' host programs too: Lube compiled into them the same way (tests/cli.sh adds the -Ds).
export LUBE_EMBED_CFLAGS="-O0 -fprofile-instr-generate -fcoverage-mapping -fprofile-update=atomic -DLUBE_NO_DISPLAY $FLAGS"

# An absolute path (some tests run lube, and programs built with it, from
# other folders), and a pool of files every run merges into under a lock (%8m):
# named by process id alone, a reused id would overwrite an earlier run's counts.
if ! LLVM_PROFILE_FILE="$PWD/$OUT/run-%8m.profraw" sh tests/run.sh "$OUT/lube" >"$OUT/tests.log" 2>&1; then
    cat "$OUT/tests.log"
    exit 1
fi
echo "tests:      $(tail -1 "$OUT/tests.log")"

"$PROFDATA" merge -sparse "$OUT"/run-*.profraw -o "$OUT/lube.profdata"
# (the graphics plugin's parts that every system has: its windows of each system's are built by CI)
SOURCES="src/*.c src/plugins/*.c src/plugins/gfx/draw.c src/plugins/gfx/font.c src/plugins/gfx/png.c"
# shellcheck disable=SC2086
"$COV" report "$OUT/lube" -instr-profile="$OUT/lube.profdata" $SOURCES >"$OUT/report.txt"
# shellcheck disable=SC2086
"$COV" show "$OUT/lube" -instr-profile="$OUT/lube.profdata" $SOURCES >"$OUT/lines.txt"

# Totals from llvm-cov: regions, functions and branches.
awk '/^TOTAL/ {
    printf "functions:  %d of %d run\n", $5 - $6, $5
    printf "regions:    %s (%d of %d)\n", $4, $2 - $3, $2
    printf "branches:   %s (%d of %d)\n", $13, $11 - $12, $11
    if ($6 != 0) exit 1
}' "$OUT/report.txt" || FUNCS_MISSED=1

# Lines, honouring the exclusion markers.
awk -v funcs_missed="${FUNCS_MISSED:-0}" '
/^\/.*\.c:$/ { file = $0; sub(/:$/, "", file); n = split(file, part, "/"); file = part[n]; excl = 0; next }
/^ *[0-9]+\|/ {
    bar = index($0, "|"); num = substr($0, 1, bar - 1) + 0
    rest = substr($0, bar + 1); bar = index(rest, "|")
    count = substr(rest, 1, bar - 1); code = substr(rest, bar + 1)
    gsub(/ /, "", count)
    if (index(code, "LCOV_EXCL_START")) excl = 1
    skip = excl || index(code, "LCOV_EXCL_LINE")
    if (index(code, "LCOV_EXCL_STOP")) excl = 0
    if (count == "") next                      # not executable
    if (skip) { excluded++; next }
    total++
    if (count == "0") { missed++; sub(/^ +/, "", code); print "  not run: " file ":" num ": " code }
}
END {
    printf "lines:      %d of %d run (%.2f%%), %d defensive lines excluded\n", total - missed, total,
           total ? 100 * (total - missed) / total : 100, excluded
    if (missed || funcs_missed) { print "coverage is below 100%"; exit 1 }
    print "coverage: 100% of lines and functions"
}' "$OUT/lines.txt"
