#!/bin/sh
# Command-line tests: flags, the REPL, standard input, files, exit codes,
# environment limits, and programs that stop with an error (each needs a run
# of its own). run.sh compares the output with tests/cli.out.
#
#   sh tests/cli.sh [path-to-lube]
cd "$(dirname "$0")/.." || exit 1
LUBE=${1:-./lube}
TMP=$(mktemp -d "${TMPDIR:-/tmp}/lube-test.XXXXXX")
trap 'rm -rf "$TMP"' EXIT

# run LABEL COMMAND...: the label, the command's output, and its exit status.
# Numbers that depend on the stack size are replaced by N.
tidy() { sed -E 's/[0-9]+ more (times|calls)/N more \1/' "$TMP/out" | sed "s|$TMP|TMP|g"; }
run() {
    echo "--- $1"
    shift
    "$@" >"$TMP/out" 2>&1
    code=$?
    tidy
    echo "[exit $code]"
}
# Like run, but feeds standard input from the first argument.
feed() {
    echo "--- $1"
    input=$2
    shift 2
    printf '%b' "$input" | "$@" >"$TMP/out" 2>&1
    code=$?
    tidy
    echo "[exit $code]"
}
status() {  # status LABEL COMMAND...: only the exit status (for exit codes)
    echo "--- $1"
    shift
    "$@" >/dev/null 2>&1
    echo "[exit $?]"
}

# ---- flags ----
run "version" "$LUBE" --version
run "short version" "$LUBE" -v
run "help" "$LUBE" --help
run "short help" "$LUBE" -h
run "unknown option" "$LUBE" -x
run "-e without code" "$LUBE" -e
run "-e prints the last value" "$LUBE" -e '1 + 2'
run "-e with arguments" "$LUBE" -e 'args' one two
run "missing file" "$LUBE" tests/does-not-exist.lube
run "file arguments" "$LUBE" tests/cli/args.lube first second
feed "script from standard input" 'print("from stdin", args)\n' "$LUBE" - a b

# ---- checking without running ----
run "check a good file" "$LUBE" --check examples/shapes.lube
run "check a file with an error" "$LUBE" --check tests/syntax_error.lube
feed "check standard input" '#strict\nfn f(x) {}\nprint("never runs")\n' "$LUBE" --check -
feed "check runs nothing" 'print("never runs")\n' "$LUBE" --check -
run "check a missing file" "$LUBE" --check tests/does-not-exist.lube
run "check without a file" "$LUBE" --check

# ---- the REPL ----
feed "repl" 'let x = 20\nx + 1\nfn twice(n) {\n  return n * 2\n}\ntwice(x)\n"text"\nprint("printed")\n[1,\n 2]\nx = 5\nundefined_thing\nlet s = "unclosed {\n}"\ns\n// a comment\n# another\nlet t = "q\\"uote"\n' "$LUBE"
feed "repl empty input" '' "$LUBE"
feed "repl line without newline" '1 + 1' "$LUBE"

# ---- input() ----
feed "input" 'ada\nbob\r\n' "$LUBE" -e 'print(input("name? ")); print(input()); try { input() } catch e { print(e) }'

# ---- files ----
run "write, append, read" "$LUBE" -e '
let f = args[0] + "/notes.txt"
write_file(f, "hello\n")
append_file(f, 42)
print(repr(read_file(f)))
try { read_file(args[0] + "/missing.txt") } catch e { print(e) }
try { read_file(args[0]) } catch e { print("reading a folder:", e) }
try { write_file(args[0] + "/no/such/dir.txt", "x") } catch e { print(e) }
try { append_file(5, "x") } catch e { print(e) }' "$TMP"
mkdir -p "$TMP/mods"
printf 'print("broken module"\n' >"$TMP/mods/broken.lube"
printf 'import "broken"\n' >"$TMP/mods/main.lube"
printf 'import "missing"\n' >"$TMP/mods/main2.lube"
printf 'fn f() { return }\nimport "bad_resolve"\n' >"$TMP/mods/main3.lube"
printf 'break\n' >"$TMP/mods/bad_resolve.lube"
printf 'print("depth", 1)\nfn boom() => 1 / 0\nboom()\n' >"$TMP/mods/fails.lube"
printf 'import "fails"\n' >"$TMP/mods/main4.lube"
run "import with a syntax error" "$LUBE" "$TMP/mods/main.lube"
run "import of a missing file" "$LUBE" "$TMP/mods/main2.lube"
run "import that does not resolve" "$LUBE" "$TMP/mods/main3.lube"
run "import that raises an error" "$LUBE" "$TMP/mods/main4.lube"

# ---- exit codes ----
status "exit(3)" "$LUBE" -e 'exit(3)'
status "exit()" "$LUBE" -e 'print("bye"); exit()'
status "uncaught error" "$LUBE" -e 'throw "x"'
status "syntax error" "$LUBE" -e 'let'

# ---- tracebacks ----
run "deep recursion" "$LUBE" -e 'fn dive(n) => n > 1000 and 1 / 0 or dive(n + 1)
fn start() => dive(0)
start()'
run "alternating recursion" "$LUBE" -e 'fn ping(n) => n > 40 and 1 / 0 or pong(n + 1)
fn pong(n) => ping(n + 1)
ping(0)'
run "long traceback ending in repeats" "$LUBE" -e 'fn z(n) => n > 0 and z(n - 1) or x(0)
fn x(n) => n > 10 and 1 / 0 or y(n + 1)
fn y(n) => x(n + 1)
z(5)'
run "uncaught non-string error" "$LUBE" -e 'fn f() { throw {code: 7} }
f()'

# ---- limits from the environment ----
run "too many items" env LUBE_MAX_ITEMS=10 "$LUBE" -e 'var l = []; for i in 0..100 { l.push(i) }'
run "too many object keys" env LUBE_MAX_ITEMS=10 "$LUBE" -e 'var m = {}; for i in 0..100 { m[i] = i }'
run "string too long" env LUBE_MAX_BYTES=1000 "$LUBE" -e 'var s = ""; for i in 0..2000 { s += "x" }'
run "concatenation too long" env LUBE_MAX_BYTES=1000 "$LUBE" -e 'let a = "x" * 600; let b = a + a'
run "printing too much" env LUBE_MAX_BYTES=1000 "$LUBE" -e 'print([12345] * 500)'
run "repeat over the limit" env LUBE_MAX_BYTES=1000 "$LUBE" -e '"ab" * 600'
run "source too large" env LUBE_MAX_BYTES=1000 "$LUBE" -e "$(printf '# x\n%.0s' $(seq 1 300))"
run "syntax tree too large" env LUBE_MAX_BYTES=1500 "$LUBE" -e "[$(printf '0,%.0s' $(seq 1 300))0]"
run "limits that are ignored" env LUBE_MAX_ITEMS=abc LUBE_MAX_BYTES=0 "$LUBE" -e 'print("ok")'
run "limit above the default" env LUBE_MAX_ITEMS=999999999999 "$LUBE" -e 'print("ok")'

# ---- programs that stop with an error: one run each ----
# tests/cli/stops.txt holds the programs, each after a "### name" line.
awk -v dir="$TMP" '
/^### / { n++; name[n] = substr($0, 5); file = dir "/case" n ".lube"; printf "" > file; next }
n > 0 { print > file }
END { for (i = 1; i <= n; i++) print i "\t" name[i] > (dir "/cases.txt") }
' tests/cli/stops.txt
while IFS="$(printf '\t')" read -r i name; do
    echo "--- $name"
    "$LUBE" "$TMP/case$i.lube" >"$TMP/out" 2>&1
    code=$?
    sed "s|$TMP/case$i.lube|program|" "$TMP/out"
    echo "[exit $code]"
done <"$TMP/cases.txt"
