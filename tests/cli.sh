#!/bin/sh
# Command-line tests: flags, the REPL, standard input, files, exit codes,
# environment limits, and programs that stop with an error (each needs a run
# of its own). run.sh compares the output with tests/cli.out.
#
#   sh tests/cli.sh [path-to-lube]
cd "$(dirname "$0")/.." || exit 1
LUBE=${1:-./lube}
TMP=$(mktemp -d "${TMPDIR:-/tmp}/lube-test.XXXXXX" | sed 's|//*|/|g')  # one slash: lube tidies paths
trap 'rm -rf "$TMP"' EXIT

# run LABEL COMMAND...: the label, the command's output, and its exit status.
# Numbers that depend on the stack size are replaced by N.
tidy() { sed -E 's/[0-9]+ more (times|calls)/N more \1/; s/, [0-9]+ bytes/, N bytes/' "$TMP/out" | sed "s|$TMP|TMP|g"; }
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

# ---- standalone executables ----
ABS="$(cd "$(dirname "$LUBE")" && pwd)/$(basename "$LUBE")"
mkdir -p "$TMP/app/lib" "$TMP/shared" "$TMP/elsewhere"
printf 'import "lib/greet"\nimport "../shared/util"\nprint(greeting("app"), shout("hi"), args)\nprint(read_file("data.txt"))\n' >"$TMP/app/main.lube"
printf 'import "../../shared/util"\nfn greeting(who: str): str => "hello from " + who\n' >"$TMP/app/lib/greet.lube"
printf 'fn shout(s) => upper(s) + "!"\n' >"$TMP/shared/util.lube"
printf 'data next to the executable\n' >"$TMP/elsewhere/data.txt"
run "build with imports" "$LUBE" --build "$TMP/app/main.lube" -o "$TMP/elsewhere/app"
mv "$TMP/app" "$TMP/app-moved"  # the executable no longer needs the sources
run "run the executable, with arguments" sh -c 'cd "$1/elsewhere" && ./app one --help -e' sh "$TMP"
printf 'print("default name", args)\n' >"$TMP/tool.lube"
run "build named after the file" sh -c 'cd "$1" && "$2" --build tool.lube && ./tool x' sh "$TMP" "$ABS"
run "build found on PATH" env PATH="/no/such/dir:$(dirname "$ABS"):$PATH" "$(basename "$ABS")" --build "$TMP/tool.lube" -o "$TMP/tool2"
run "run a build found on PATH" env PATH="$TMP:$PATH" tool2 y
printf 'let x = (\n' >"$TMP/broken.lube"
printf 'import "missing"\n' >"$TMP/needs.lube"
printf 'print(1)\n' >"$TMP/noext"
run "build a file with errors" "$LUBE" --build "$TMP/broken.lube" -o "$TMP/x"
run "build with a missing import" "$LUBE" --build "$TMP/needs.lube" -o "$TMP/x"
run "build a missing file" "$LUBE" --build "$TMP/none.lube" -o "$TMP/x"
run "build that would replace its source" sh -c 'cd "$1" && "$2" --build noext' sh "$TMP" "$ABS"
run "build to a folder that does not exist" "$LUBE" --build "$TMP/tool.lube" -o "$TMP/no/such/dir/app"
run "build with bad options" "$LUBE" --build "$TMP/tool.lube" -x out
run "build when lube cannot find itself" perl -e 'exec {shift} "no-such-lube", @ARGV' "$ABS" --build "$TMP/tool.lube" -o "$TMP/x"

# ---- isolates (several write at once, so their lines are sorted) ----
run "isolates that fail with nobody joining, and one left waiting at the end" sh -c '"$1" -e "$2" 2>&1 | sort' sh "$ABS" 'fn boom(n) => 1 / n
fn waits(c: Channel) { c.receive() }
spawn(boom, 0)
let kept = spawn(boom, 0)
let c = channel()
spawn(waits, c)
print("main done")'
run "an isolate already waiting when the program ends" sh -c '"$1" -e "$2" 2>&1 | sort' sh "$ABS" 'fn waits(ready: Channel, c: Channel) {
  ready.send(1)
  c.receive()
}
let ready = channel()
let c = channel()
spawn(waits, ready, c)
ready.receive()
var spin = 0
while spin < 3000000 { spin += 1 }
print("main done")'
run "an isolate failing while it starts" "$LUBE" -e 'fn sq(n) => n * n
class Starter {
  var h = spawn(sq, 2)
}
try { spawn(sq, 3).join() } catch e { print(e) }'
printf 'import "lib/double"\nfn work(n: Number): Number => double(n) + 1\nprint(parallel_map([1, 2, 3], work), args)\n' >"$TMP/iso.lube"
mkdir -p "$TMP/lib"
printf 'fn double(n: Number): Number => n * 2\n' >"$TMP/lib/double.lube"
run "a built program with isolates" sh -c '"$1" --build "$2/iso.lube" -o "$2/iso" >/dev/null && rm -r "$2/lib" && cd / && "$2/iso" x y' sh "$ABS" "$TMP"
feed "isolates from the REPL" 'fn sq(n) => n * n\nspawn(sq, 9).join()\n' "$LUBE"

# ---- packages ----
SHAPES="$PWD/tests"
mkdir -p "$TMP/pkgs/mixed" "$TMP/pkgs/bare" "$TMP/pkgs/broken" "$TMP/pkgs/boomy" "$TMP/pkgs/unresolved" "$TMP/pkgs/live"
printf 'package mixed\nfn one() => 1\n' >"$TMP/pkgs/mixed/one.lube"
printf 'package other\nfn two() => 2\n' >"$TMP/pkgs/mixed/two.lube"
printf 'fn three() => 3\n' >"$TMP/pkgs/bare/three.lube"
printf 'package broken\nlet x = (\n' >"$TMP/pkgs/broken/bad.lube"
printf 'package boomy\nfn f() => 1\nlet x = 1 / 0\n' >"$TMP/pkgs/boomy/b.lube"
printf 'package unresolved\nfn f() => 1\nbreak\n' >"$TMP/pkgs/unresolved/u.lube"
printf 'package live\nfn version() => 1\n' >"$TMP/pkgs/live/live.lube"
run "import a missing name" env LUBE_PATH="$SHAPES" "$LUBE" -e 'import shapes.Nope'
run "import a private name" env LUBE_PATH="$SHAPES" "$LUBE" -e 'import shapes._secret'
run "import a missing package" env LUBE_PATH="/no/such/dir:$SHAPES" "$LUBE" -e 'import nowhere.Thing'
run "import * from a missing package" "$LUBE" -e 'import nowhere.*'
run "import a package with no dots" "$LUBE" -e 'import nowhere'
run "a package import inside a function" env LUBE_PATH="$SHAPES" "$LUBE" -e 'fn f() {
  import shapes.Point
}'
run "a package line that is not first" "$LUBE" -e 'print(1)
package late'
run "a package with files of two packages" env LUBE_PATH="$TMP/pkgs" "$LUBE" -e 'import mixed.*'
run "a package file without its package line" env LUBE_PATH="$TMP/pkgs" "$LUBE" -e 'import bare.*'
run "a package with a syntax error" env LUBE_PATH="$TMP/pkgs" "$LUBE" -e 'import broken.*'
run "a package that fails as it loads" env LUBE_PATH="$TMP/pkgs" "$LUBE" -e 'import boomy.f'
run "a package that does not resolve" env LUBE_PATH="$TMP/pkgs" "$LUBE" -e 'import unresolved.f'
run "a string import of a package file" sh -c 'cd "$1" && "$2" -e "import \"shapes/point\"
print(Point(1, 2))"' sh "$SHAPES" "$ABS"
feed "package imports in the REPL" 'import shapes.Point\nPoint(1, 2)\n' env LUBE_PATH="$SHAPES" "$LUBE"
mkdir -p "$TMP/papp"
cp -R "$SHAPES/shapes" "$TMP/papp/shapes"
printf 'import shapes.extra.label\nimport shapes.Point\nfn tag(n: Number): str => label(Point(n, n))\nprint(parallel_map([1, 2], tag))\n' >"$TMP/papp/main.lube"
run "a built program with packages" sh -c '"$1" --build "$2/papp/main.lube" -o "$2/papp/app" >/dev/null && rm -r "$2/papp/shapes" && cd / && "$2/papp/app"' sh "$ABS" "$TMP"
run "hot reload of a package file" env LUBE_PATH="$TMP/pkgs" "$LUBE" -e 'import live
print(live.version())
write_file(args[0], "package live\nfn version() => 2\nfn added() => \"new\"\n")
hot_reload()
print(live.version(), live.added())' "$TMP/pkgs/live/live.lube"

# ---- the terminal debugger (lube --debug) ----
D=tests/cli/debug_app.lube
feed "debugger: breakpoints, values, frames, stepping" 'h\nbl\nb nope\nb 13 if item.price > 3\nc\np item\np sum + 1\np nothing_here\np 1 +\np 1; 2\np print("side effect")\nlocals\nglobals\nbt\nup\nlocals\nframe 9\nframe 0\ndown\nl\nn\nn\nbl\nd 7\nd 1\nlog 28 mapping {i.name}\ntb 28\nc\ns\nlocals\no\nb 22\nc\nlocals\ns\nlocals\nwhat\n\nd\ncatch bogus\ncatch all\nc\nbt\ncatch off\nc\n' "$LUBE" --debug "$D"
feed "debugger: an uncaught error stops where it is raised" 'c\np n\nbt\nc\n' "$LUBE" --debug "$D"
feed "debugger: hit counts, a broken condition, and quitting" 'b 13 hits 2\nb 13 if nope\nc\nc\np item.name\np sum = 5\nd\nb tests/cli/debug_app.lube:13 hits % 3\nc\np item.name\nq\n' "$LUBE" --debug "$D"
printf 'import shapes.Point\nlet p = Point(1, 2)\nprint(p)\n' >"$TMP/uses_shapes.lube"
feed "debugger: expressions see the file's imports" 'n\np Point(3, 4)\np p.x\nc\n' env LUBE_PATH="$SHAPES" "$LUBE" --debug "$TMP/uses_shapes.lube"
feed "debugger: no more commands runs the program on" 'b 13\nc\n' "$LUBE" --debug "$D"
feed "debugger: in an on block in a function" 'b 7\nc\nlocals\np gain * bonus + score\nbt\nup\nlocals\nn\nn\nn\nn\nc\n' "$LUBE" --debug tests/cli/debug_on.lube
printf 'var i = 0\nwhile true {\n  i += 1\n}\n' >"$TMP/spin.lube"
printf 'c\np i > 0\nq\n' >"$TMP/spin.cmds"
echo "--- debugger: Ctrl-C pauses a running program"
"$LUBE" --debug "$TMP/spin.lube" <"$TMP/spin.cmds" >"$TMP/out" 2>&1 &
pid=$!
sleep 1
kill -INT "$pid"
wait "$pid"
code=$?
sed "s|$TMP|TMP|g" "$TMP/out" | sed -E 's/spin\.lube:[0-9]+/spin.lube:N/; s/^    [0-9] \| .*/    N | (a line of the loop)/'
echo "[exit $code]"
run "debugger without a file" "$LUBE" --debug

# ---- hot reload ----
mkdir -p "$TMP/hot" "$TMP/hotloop"
cp tests/cli/hot.lube "$TMP/hot/main.lube"
cp tests/cli/hot_loop.lube "$TMP/hotloop/main.lube"
run "hot_reload()" sh -c 'cd "$1" && "$2" main.lube' sh "$TMP/hot" "$ABS"
run "lube --hot" sh -c 'cd "$1" && "$2" --hot main.lube a b' sh "$TMP/hotloop" "$ABS"
run "lube --hot without a file" "$LUBE" --hot

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
printf 'let LIMIT = 5\n' >"$TMP/mods/consts.lube"
printf 'import "consts"\non LIMIT { print(LIMIT) }\n' >"$TMP/mods/main5.lube"
run "on a let from an import" "$LUBE" "$TMP/mods/main5.lube"

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
