# Lube

Lube is a small scripting language with a smooth syntax: no semicolons, string interpolation, closures, classes, a `|` pipe operator, and method-style calls on everything. Every value carries its type, a `var` keeps the type it starts with, and there is no null.

It manages memory **without a garbage collector**. It does this without lifetimes or a borrow checker either: every value has exactly one owner, and memory is freed the moment that owner lets go.

```lube
let words = "the quick brown fox jumps over the lazy dog".split()

let longest = words
  | filter(fn(w) => len(w) > 3)
  | map(fn(w) => w.upper())
  | sort()

print("long words: {longest.join(", ")}")   # long words: BROWN, JUMPS, LAZY, OVER, QUICK
```

The interpreter is written from scratch in C99. It uses only the C standard library: no parser generators, no third-party code, no runtime of another language.

## Build and run

```sh
make            # builds ./lube, and its plugins in ./plugins (needs only a C compiler)
make lib        # builds liblube.a, for running Lube inside a C or C++ program (see Embedding)
make test       # runs the test suite
make check      # runs it again with memory sanitizers and a leak check
make coverage   # measures test coverage (needs clang's llvm-cov); fails below 100%
make tsan       # runs the suite with ThreadSanitizer (data races between isolates)
make browser    # runs the web plugin's pages in headless Chrome (needs Node 22+ and Chrome)
tests/soak/run.sh   # runs everything for an hour, watching memory and speed (see Testing)
make pgo        # builds lube twice, the second time laid out by how the first one ran (needs clang): 5-15% faster
make install    # copies lube to /usr/local/bin and its plugins to /usr/local/lib/lube (PREFIX=...)
```

It builds and passes the tests on Linux (gcc or clang), macOS and Windows. On Windows, build in an [MSYS2](https://www.msys2.org) UCRT64 shell: `pacman -S make diffutils mingw-w64-ucrt-x86_64-gcc`, then `make`. GitHub Actions runs everything above on each pull request, and each release (a `v*` tag) comes with ready-built binaries: a static Linux x86-64 binary (with its plugins inside: `make STATIC_PLUGINS=1`), and a universal macOS binary (Apple silicon and Intel) and a Windows `lube.exe`, each with its `plugins/` folder. A binary downloaded with a browser on macOS isn't signed: clear the quarantine flag with `xattr -d com.apple.quarantine lube` before running it.

```sh
./lube                       # interactive REPL
./lube examples/hello.lube   # run a file
./lube script.lube a b c     # extra arguments are in the `args` list
./lube -e 'print(2 ** 10)'   # run a snippet
./lube --check app.lube      # report syntax errors without running (exit 0 or 65)
./lube --build app.lube      # build a standalone executable ./app (or: -o name)
./lube --hot app.lube        # run, applying your edits to the running program
echo 'print("hi")' | ./lube -
```

### Shipping a program: script or executable

A Lube program can be run as a script, or built into a single executable that runs without Lube installed:

```sh
./lube app.lube one two            # as a script
./lube --build app.lube -o app     # built: app.lube and every file it imports
./app one two                      # the same program; args is ["one", "two"]
```

The executable is a copy of the interpreter with the program appended, so it starts as fast as a script and behaves the same. Building checks every file first, so a program with a syntax error, or an import that can't be found, doesn't build. Imports are packed with the paths the program uses, so the sources aren't needed afterwards. Files the program reads with `read_file` are still read from disk, relative to the current folder. Every command-line argument goes to the program: a built executable has no options of its own. A program that imports plugins gets them in a `plugins/` folder next to it, to ship together; with `--small` or `--native`, Lube's own plugins go inside the executable instead.

### Native code: `lube --build --native`

```sh
./lube --build --native app.lube -o app
native: fib, collatz, mandel
built app: 1 file, 339570 bytes
```

`--native` also compiles the program's functions that compute only with Numbers and Bools to machine code, through the system's C compiler. A function qualifies when its result is typed `Number` or `Bool` (or it returns nothing), each parameter is typed `Number` or `Bool` or is a record (below), and its body uses only:
- arithmetic, comparisons, `and`, `or` and `not`;
- `if`, `while`, and `for` over a range (`0..n`, `1..=n`);
- local `var`s and `let`s;
- lists of Numbers or of Bools that the function makes itself (`[0] * n`, `Number[]`, `[a, b]`), and indexes, changes, pushes to and measures with `len`, but never passes on or returns;
- records: an untyped parameter that the function uses as an object or class instance (`p.x`), or as a list of them (`bodies[i].vx`, `for b in bodies`, `len(bodies)`), reading and changing their Number fields (up to 16 of them). Copies (`let b = bodies[i]`) work too. As on the VM, changes reach the caller only through a `&` parameter;
- values: an untyped parameter that isn't a record holds any value, read with `len(t)` and `t[i]` (lists of lists, a list of Numbers from the VM...), passed on, and turned into a Number or Bool where one is needed; a list literal makes a new one (`[left, right]`), and a function without a written result type returns one;
- calls to other such functions (passing records on, with `&` or without) and to the math built-ins (`sqrt`, `abs`, `floor`, `pow`, `log`...);
- method calls on a record, when the method qualifies too and only one class has a method of that name.

Methods qualify the same way, `this` being a record. So do loops at the top level of a file (`for` or `while`): the globals they use (Numbers, Bools, records, and lists of Numbers or of Bools) are their parameters, taken in when the loop starts and written back when it ends.

The build lists what it compiled, and for each typed function it didn't, the line where it stopped (`not native: names (line 12)`). Everything else runs on the VM as usual.

```lube
fn fib(n: Number): Number {      // compiled
  if n < 2 { return n }
  return fib(n - 1) + fib(n - 2)
}
fn names(n: Number): str => "{n}"   // not compiled (text): runs on the VM
```

```lube
class Body {
  var x = 0
  var vx = 1
  fn move(dt: Number) { this.x += this.vx * dt }   // compiled: a method
}
fn advance(&bodies, dt: Number) {   // compiled: a list of records
  for i in 0..len(bodies) { bodies[i].move(dt) }
}
var bodies = [Body(), Body()]
for step in 0..1000 { advance(&bodies, 0.01) }   // compiled: a top-level loop
```

The machine code computes exactly what the VM would, down to the last bit of every Number. A function given only whole numbers runs on 64-bit integers wherever its numbers stay whole, and switches to doubles where they don't or past 2^53. The machine code never reports an error itself. When one would happen (division by zero, a function that ends without returning its value, recursion too deep, a record without the field used, a field that isn't a Number, an object of another class than its methods'), the VM runs that call or loop again from the start and reports it as usual, with the usual traceback. That's safe because nothing outside changes until the machine code is done: a function's changes to its `&` records, and a loop's to its globals, are written back only when it finishes. A loop changing a global that an `on` block watches runs on the VM, so the block runs on each change.

`--native --fast` trades that exactness for speed, the way C and Rust's release builds do: whole numbers are 64-bit integers that wrap around past 2^63 instead of turning into doubles past 2^53, and the C compiler may reorder sums of doubles (so that it can use SIMD instructions), which can change their last digits. Errors still come from the VM as usual (division by zero, an index out of range, recursion too deep). Lube's own code stays exact: only the compiled functions change.

```sh
./lube --build --native --fast app.lube -o app
```

It needs a C compiler (`cc`, or `CC`; `gcc` on Windows) when building, not when running, and Lube's C sources: the `src` folder next to `lube`, or the folder `LUBE_SRC` names. On the benchmarks in `bench/vs_rust`, the native builds run `fib` 25 times faster than the VM, the arithmetic `loop` 11 times faster, the `sieve` (a list of Bools) 27 times faster in a twelfth of the memory, `nbody` (a list of class instances, stepped by a top-level loop) 58 times faster, `methods` (a method called five million times) 44 times faster, and `trees` (building and walking lists of lists) 6 times faster. That's 1.7, 2.2, 1.3, 1.3, 1.5 and 0.7 times the time of the same programs in Rust, and `sort` (a top-level loop filling a list of a million Numbers, then `sort()`) takes 1.2 times as long. Over all ten benchmarks, the geometric mean is 1.35 times Rust's time with `--native`, and 1.2 times with `--native --fast` (`python3 bench/vs_rust/run.py --native [--fast]`).

### Small executables: `lube --build --small`

```sh
./lube --build --small app.lube -o app
small: 10 of 117 built-ins, 7 of 13 fns and classes
built app: 3 files, 162557 bytes
```

For small machines such as a Raspberry Pi, `--small` builds an executable with only the parts of Lube, and of the program, that can run:
- The program goes in compiled, so the executable has no lexer, parser or compiler, and starts without compiling anything.
- The functions and classes that nothing can reach are left out.
- Of Lube's built-in functions, only those the program names go in.
- What only the `lube` command needs (the REPL, the debugger, the tools that build programs and compile them to machine code) stays out, and the rest is optimized for size.

On macOS (Apple silicon), `hello.lube` is 142 KB instead of 388 KB. A 15,000-line program that calls one of its 3,000 functions is 156 KB instead of 653 KB, and it starts 8 times faster, in 1.7 MB of memory instead of 24.5 MB.

The build finds what can run by name: the files' top-level statements, and the functions and classes they reach (a class with all its methods). A program that calls `globals()` (which reaches any global by name) keeps everything; one that calls `find_class` or `classes()` keeps every class. The sources go in too, without the lines of what was left out, so error messages show the lines they point at. Lube's own code is compiled for size, so it runs a little slower; with `--native` too, the compiled functions keep their speed.

Like `--native`, it needs a C compiler when building (Raspberry Pi OS comes with `gcc`) and Lube's C sources (the `src` folder next to `lube`, or `LUBE_SRC`).

## Memory: no garbage collector, no drama

Lube gives every value **value semantics**, the way numbers already behave in every language:

```lube
var a = [1, 2, 3]
var b = a          # b gets its own copy
b.push(4)
print(a, b)        # [1, 2, 3] [1, 2, 3, 4]
```

What follows from that:

- **Copies are cheap.** `b = a` doesn't copy anything yet. Both share the data until one of them changes it, and only then does the changer get its own copy (copy-on-write).
- **Short text needs no memory of its own.** A str of up to 10 bytes is held inside its value, as a Number is. Making one, copying it, comparing two and letting go of one touch no other memory, so the words of a split line, the characters of a str and keys like `"id"` or `"k42"` cost what numbers cost. Longer text is shared and counted like every other value, and a program can't tell the two apart.
- **Values can never form cycles.** A value can't contain itself (`box.self = box` stores a *copy* of `box`), closures capture copies (see below), and class instances are values too. Without cycles, a simple owner count is enough: when the last owner lets go, the memory is freed immediately.
- **Memory is freed deterministically.** Values are freed at the end of a block, when a variable is overwritten, or when a function returns. There's no collector running in the background, no pauses, and no leaks.
- **Iteration can't be broken.** A loop over a list keeps seeing the list as it was when the loop started, even if the body changes the variable.

Lube programs can't corrupt memory: there are no pointers, every index is bounds-checked, integer arithmetic is overflow-checked, and nothing is ever freed by hand. Extreme input gets a clear error instead of a crash. That covers numbers like `1e300` or NaN used as sizes, code nested thousands of levels deep, and runaway recursion. A string can hold at most 1 GB and a list, object or map at most 268 million items, so one expression can't claim all of the machine's memory. To run untrusted scripts with less, set `LUBE_MAX_BYTES` (the longest string, and the largest source file) and `LUBE_MAX_ITEMS` (the most items in one list, object or map). They can lower the limits but never raise them. A program that goes over a limit stops with `lube: out of memory`.

The interpreter itself is written in C, so its safety rests on testing. `make check` runs the whole suite under AddressSanitizer and UndefinedBehaviorSanitizer and verifies at exit that every object was freed. `make coverage` checks that the suite runs every line and every function of the interpreter. The interpreter has also been fuzzed with tens of thousands of malformed and adversarial programs, and a soak test runs every part of Lube for an hour while watching its memory (see Testing).

## Language tour

### Variables: `let` and `var`

```lube
let name = "Lube"     # let: can never change
var count = 0         # var: can change
count += 1

name = "other"        # error, before the program even runs:
                      # cannot change 'name': it was declared with let
```

`let` freezes the whole value, so a `let` list can't be pushed to and a `let` object's fields can't be set. Function names, loop variables and caught errors also can't be reassigned.

A `var` can change its value, but not its type:

```lube
var count = 0
count = 10            // fine
count = "ten"         // error: 'count' is a Number, so it cannot hold a str:
                      //        a var keeps the type it starts with

var name = String()   // a String, starting as ""
name = ""             // error: 'name' is a String, so it cannot hold a str
                      //        ("" is a str: write String(""), see Text below)
```

A `var` that starts as an `Object` accepts any object, and one that starts as a class instance accepts instances of its subclasses. A plain whole number adapts to a sized integer `var`, so `count = 5` works for a `uint32` count.

Comments start with `#` or `//`. Statements end at the end of a line. Use `;` to put several on one line. A line that starts with `.`, `|`, `and` or `or` continues the previous one, and so does any line inside `( )` or `[ ]`.

### Values

| type       | examples                                 |
|------------|------------------------------------------|
| `Bool`     | `true`, `false`                          |
| `Number`   | `42`, `3.14`, `1_000_000`, `0xff`, `1e-3` (64-bit floats) |
| sized integers | `int32()` (0), `int64(23)`, `uint8(255)`: see below |
| `str`      | `"hello {name}"`, `'no {interpolation}'` (plain text) |
| `String`   | `String("hello")` (text with properties: see below) |
| `List`     | `[1, "two", [3]]`, or typed: `str["a", "b"]` |
| `Object`   | `{name: "Ada", "with space": 1, [1 + 1]: "computed key"}` |
| `Map`      | `Map[str, Number]`: keys of any type |
| `Range`    | `0..10` (excludes 10), `1..=10` (includes 10) |
| `Function` | `fn(x) => x * 2`                         |
| classes    | `Dog("rex")`: see Classes below          |
| `Channel`, `Isolate` | `channel()`, `spawn(f, x)`: see Concurrency below |
| `Bytes`    | `Bytes("hi")`, `Bytes(16)`: see Bytes and time below |
| `Money`    | `m(19.99)`, `m(5, "EUR")`, `m(5, "₱")`: see Money below |
| plugins' types | `File`, `Socket`, `Sound`, `Image`, `Complex`, `Array`, `Tensor`...: see Plugins below |

`Bool`, `Number`, `str` and the sized integers are *primitives*. Everything else is an object, so `x is Object` is true for a list, a map, a `String` or a class instance, and false for `5` or `"text"`.

Only `false` is falsy. `0`, `""` and `[]` are all truthy. There is no `nil`: see below.

### No null

Lube has no `nil`, `null` or "undefined". Every value you can hold is a real value, so a missing value can never quietly travel through a program and fail somewhere far away.

```lube
var count = 0                        // every variable starts with a value
let user = {name: "ada"}

user.email                           // error: the object has no field 'email'
user.has("email")                    // false
get(user, "email", "none")           // "none"

num("abc")                           // error: 'abc' is not a number
num("abc", 0)                        // 0: a fallback you chose

fn greet(who) { print("hi", who) }   // returns nothing
greet("bob")                         // fine as a statement
let r = greet("eve")                 // error: greet() does not return a value
```

| where other languages give null | Lube |
|---|---|
| a variable declared without a value | not allowed: `var x` needs a starting value |
| a missing object key | an error; check with `has()` or use `get(object, key, default)` |
| a function that returns nothing | can be called, but its result can't be stored, passed or printed |
| text that isn't a number | an error, or `num(text, fallback)` |
| no item matches a search | `find_index` gives -1; `find(xs, f, default)` |
| end of input | `input()` raises an error you can catch |

### Types

Every value has a type, and `type(x)` gives its name: `str`, `String`, `Number`, `Bool`, `Object`, `List`, `Map`, `Function`, `Range`, `Bytes`, `Money`, the sized integers such as `int32`, typed lists such as `String[]`, typed maps such as `Map[str, Number]`, and class names. `x is T` checks a type, including a class and its parents. Type names do three things:

```lube
var o = Object               // a bare type name is its default value: {}
var s = String("Hello")      // Type(x) converts a value
var arrstring = String[]     // Type[] is an empty list that only holds that type
var ids = int32[1, 2, 3]     // ...or one that starts with items
var maps = Map[Object, String]   // a map with typed keys and values
```

| type | default | conversion |
|---|---|---|
| `str` | `""` | `str(42)` is `"42"`; `str(s)` takes a `String`'s text |
| `String` | `String("")` | `String(42)` is `String("42")` |
| `Number` | `0` | `Number("3.5")`; `Number("x", 0)` falls back to 0 |
| `Bool` | `false` | `Bool(x)` is `x`'s truthiness |
| `Object` | `{}` | `Object([["a", 1], ["b", 2]])` builds one from pairs |
| `Map` | an empty map | `Map([[1, "a"], [[2], "b"]])` builds one from pairs |
| `List` | `[]` | `List("abc")` is `["a", "b", "c"]` |
| `Range` | `0..0` | |
| `int32` (and the others) | `0` | `int32("42")`, with a range check |
| `Function` | none | `Function(f)` checks that `f` is a function |
| a class | `Dog()` | `Dog(...)` runs its constructor |

Conversion is never implicit: a `str` doesn't turn into a `String` or back on its own, and neither do numbers and text. `x.T()` is the same as `T(x)`, so `"5".String()` works too.

A typed list keeps its promise: every item always has its type. A `str[]` refuses a number whether it arrives by `push`, `insert`, `xs[i] = v`, `+=`, `+`, a `str[...]` literal, or a function that changes an element through `&`. Plain whole numbers adapt to integer lists, so `ids.push(5)` works for an `int32[]`, but nothing else converts: a `String[]` takes `String("a")`, not `"a"`. Lists derived from a typed list keep its type: `names.sort()`, `slice`, `filter`, `reverse`, `+` and `*` all return `str[]`. `List(x)` makes a plain list that accepts anything.

```lube
var names = str["ada"]
names.push("bob")            // fine
names.push(5)                // error: this str[] can only hold str values, not a Number
```

Type names are reserved, so `let String = 1` is an error.

### Sized integers

```lube
let number1 = int32()       // default 0
let number2 = int64(23)
var count = uint32(0)
count += 1                  // a plain whole number takes the integer's type
print(number2 * 2, type(number2))   // 46 int64
```

There are eight types: `int8 int16 int32 int64 uint8 uint16 uint32 uint64`. With no argument each gives 0. Each converts a number (dropping any fraction), a numeric string such as `"42"`, `"0xff"` or `"1_000"`, a bool, or another integer type.

They never silently go wrong:

- A value that doesn't fit is an error: `int8(300)`, `uint8(-1)`.
- Arithmetic is checked: `int32(2147483647) + 1` is an overflow error, not a wraparound.
- Different integer types don't mix: `int32(1) + int64(1)` asks you to convert one. A fractional number doesn't mix either: `int32(5) * 1.5` suggests `num(x)`.
- `/` divides and rounds down, and `%` takes the divisor's sign: `int32(-7) / 2` is `-4`, and `int32(-7) % 2` is `1`.
- Comparisons and `==` work across all number types by exact value: `int32(5) == 5` and `int8(-1) < uint64(0)` are both true. Equal numbers are the same object key.

Plain numbers are exact only up to 2⁵³, so write larger 64-bit values as strings: `int64("9223372036854775807")`.

### Text: `str` and `String`

Lube has two text types. `str` is plain, primitive text: every string literal is a `str`. A `String` is an object holding text, with read-only properties:

```lube
let s = String("héllo world")
print(s.length)      // 11: characters (len(s) counts bytes: 12)
print(s.upper)       // HéLLO WORLD: upper and lower change ASCII letters only
print(s.words)       // ["héllo", "world"]
print(s.chars)       // ["h", "é", "l", ...]
print(s.value)       // the text as a str
```

Both work with every text function (`split`, `upper`, `replace`, ...), and they compare equal when their text is the same: `String("a") == "a"` is true. They are still different types, so a `String` variable can't take a `str` (write `String("...")`), and a `str` variable can't take a `String` (use `s.value`).

```lube
let who = "world"
print("Hello, {who}! 2 + 2 = {2 + 2}")    # interpolation runs any expression
print('single quotes: {not interpolated}')
print("escapes: \n \t \" \\ \{ \u{2764} \x41")
print("Lube"[0], len("Lube"), "a,b".split(","), "Hi".upper())
```

`+` joins text with anything: `"n = " + 5` gives `"n = 5"`.

### Operators

| precedence (low to high) | operators |
|---|---|
| assignment | `=` `+=` `-=` `*=` `/=` `%=` |
| pipe | `\|` |
| logic | `or` / `\|\|`, `and` / `&&`, `not` |
| equality | `==` `!=` (lists and objects compare by contents) |
| comparison | `<` `<=` `>` `>=` `in` |
| range | `..` `..=` |
| arithmetic | `+` `-`, then `*` `/` `%` `@`, then unary `-` `!`, then `**` |

The pipe passes its left side as the first argument: `x | f(y)` is `f(x, y)`, and `x | f` is `f(x)`.

`%` follows the sign of the divisor (`-7 % 3` is `2`), and dividing by zero is an error.

`@` is the matrix product of Arrays and Tensors (the plugins `array` and `nn`): Lube's own values have none. A plugin's types can have any of the operators, and indexing with several indexes, `a[i, j]`, which lists, text and objects don't take. In brackets a range may leave out its start or its end: `a[2..]` runs to the end, `a[..3]` starts at 0, and `a[.., 1]` takes all of the first axis (`a..` before `,` or `]` is `a..inf`).

### Control flow

```lube
if score >= 90 {
  print("A")
} else if score >= 80 {
  print("B")
} else {
  print("keep going")
}

while running { ... }            # break and continue work as usual

for i in 0..5 { ... }            # ranges
for item in list { ... }
for i, item in list { ... }      # index and item
for key, value in object { ... }
for ch in "héllo" { ... }        # characters (UTF-8 aware)
```

### Functions

```lube
fn add(a, b) {
  return a + b
}

fn square(x) => x * x                  # one-expression form

fn greet(name, greeting = "Hello") {   # default parameters
  return "{greeting}, {name}!"
}

let twice = fn(f, x) => f(f(x))        # functions are values
print(twice(square, 3))                # 81
```

Parameters can't be reassigned unless you mark them:

```lube
fn countdown(var n) {        # var: a changeable local copy
  while n > 0 { n -= 1 }
}

fn add_tax(&price) {         # &: changes the caller's variable
  price = price * 1.2
}
var cost = 100
add_tax(&cost)               # cost is now 120
cost.add_tax()               # method style passes &cost automatically
```

`&` lends the variable to the function: the value is moved in (no copy), changed in place, and moved back when the function returns. Changes are kept even if the function raises an error. The language checks that `&` is used exactly where a function changes its argument.

### Typed functions and `#strict`

Parameters and return values can declare types, in functions, methods and lambdas alike:

```lube
fn scale(by: Number, &shape: Rect): Bool { ... }
fn greet(name: str = "you"): str => "hi " + name
fn next(n: int32): int32 => n + 1          // next(5) is int32(6)
let double = fn(x: Number): Number => x * 2
```

A type is any type name (`str`, `String`, `Number`, `Bool`, `Object`, `Map`, `List`, `Function`, `Range`, the sized integers), a typed list such as `str[]`, `Map[K, V]`, or a class name. They are checked on every call:

- An argument of the wrong type is refused at the call: `scale("x")` gives *parameter 'by' of scale() is a Number, so it cannot take a str*.
- The result must have the return type, and a function with a return type must return a value on every path: *forgets() must return a Number, but it ended without returning a value*.
- `List` and `Map` accept any list or map, the way `x is List` does. `str[]` and `Map[str, Number]` accept only that exact kind, and a class accepts its subclasses.
- A plain whole number becomes the integer type a parameter or result asks for. Through `&` nothing converts: the caller's variable must already have the type.
- A typed `var` parameter keeps its declared type, so `var o: Object` can later hold any object.

Without types, a function accepts and returns anything, as before. To make types required, start a file with the line `#strict`:

```lube
#strict
fn area(w: Number, h: Number): Number => w * h
fn report(name: str) {                    // no return type: returns nothing
  print(name, area(2, 3))
}
```

In a `#strict` file every parameter needs a type, and a function without a return type returns nothing (`return value` in it is an error). `#strict` must come before any code (comments are fine) and applies to that file only, not to the files it imports. Run `lube --check file.lube` to find these errors without running the program.

### State and objects

State lives in plain values. Functions that change state take it with `&`, and method syntax supplies it:

```lube
fn account(owner) => {owner: owner, balance: 0}
fn deposit(&acct, amount) { acct.balance += amount }

var acct = account("ada")
acct.deposit(50)              # same as deposit(&acct, 50)
print(acct.balance)           # 50
```

If an object has a key with the method's name, `obj.key()` calls the function stored there instead. A class instance's own methods come first.

### Closures

A closure captures a **copy** of each local variable it uses, made when the closure is created:

```lube
fn make_multiplier(factor) => fn(x) => x * factor
let triple = make_multiplier(3)
print(triple(7))     # 21
```

Closures can read what they captured but not change it, because it's their own copy. Trying to is an error that explains this. Put changing state in a value and pass it with `&` instead.

Top-level variables are globals, which every function sees live. A global `var` is the simple way to keep script-wide state, and top-level functions can call each other in any order.

### Lists and objects

```lube
var xs = [1, 2, 3]
xs.push(4)
xs[0] = 10
print(xs[-1], len(xs), xs.slice(1, 3), 2 in xs)

var user = {name: "Ada", tags: []}
user.email = "ada@example.com"          # add or change a field
user.tags.push("admin")                 # change a nested value in place
print(user.name, user["email"], get(user, "phone", "none"))   # user.phone alone is an error
```

Objects remember insertion order. Keys can be `str`, numbers or booleans. Reading a key that isn't there is an error, so use `has(obj, key)` or `get(obj, key, default)` when it might be missing.

A **map** is like an object, but its keys can be any value, including lists and objects, compared by contents (all but `nan`, which is not equal to itself: storing under it is an error). `Map[K, V]` makes one that checks its key and value types:

```lube
var scores = Map[str, Number]
scores["ada"] = 3
scores[1] = 2            // error: this Map[str, Number] has str keys, not a Number
scores["bob"] = "high"   // error: this Map[str, Number] can only hold Number values, not a str

var seen = Map           // any keys and values
seen[[1, 2]] = "pair"
print(seen[[1, 2]], keys(scores), values(scores))   // pair ["ada"] [3]
```

`keys` and `values` of a typed map are typed lists (`str[]` and `Number[]` here), and `for key, value in map` works as it does for objects.

### Classes

Every class is an object type: `Object` is the base of all classes.

```lube
class Animal {
  var name = "animal"              // fields, each with a default value
  var legs = 4
  fn speak() => "{this.name} makes a sound"
  fn rename(n) { this.name = n }   // methods can change this
}

class Dog extends Animal {
  var tricks = str[]
  constructor(name) {              // optional; runs on Dog(...)
    super()                        // the parent's constructor
    this.name = name
  }
  fn speak() => super.speak() + ": woof"
  fn learn(trick) { this.tricks.push(trick) }
}

var rex = Dog("rex")
rex.learn("sit")
print(rex)                  // Dog {name: "rex", legs: 4, tricks: ["sit"]}
print(rex.speak())          // rex makes a sound: woof
print(rex is Animal)        // true
```

- **Fields are fixed and typed.** An instance has exactly the fields its class declares, and each keeps the type of its default: `rex.age = 3` and `rex.name = 5` are both errors.
- **Instances are values.** `var other = rex` copies (cheaply, with copy-on-write), so changing `other` leaves `rex` alone. Instances compare by contents.
- **A method can change `this` only when its receiver can change.** `rex.rename("max")` works on a `var`, but on a `let` it is an error.
- A bare class name is a default instance (`var pet = Animal` calls `Animal()`), and a `var` holding an `Animal` also accepts a `Dog`.
- Methods and constructors can declare parameter and return types like any function (see Typed functions above). `this` never needs one.
- `delete` and `clear` refuse instances: an instance always has every field its class declares.

### Reflection

A program can look at its own classes, functions and globals:

```lube
var rex = Dog("rex")
let C = class_of(rex)              // the class itself: prints as <class Dog>
print(C.name, type(C))             // Dog Class
print(parent_of(C))                // <class Animal>
print(fields(rex))                 // [{name: "name", type: "str"}, {name: "legs", type: "Number"}, ...]
print(methods(C))                  // ["speak", "learn", "rename"]: inherited ones too

fn scale(by: Number, &shape: Rect): Bool => ...
print(signature(scale))
// {name: "scale", params: [{name: "by", type: "Number", mode: "let", has_default: false},
//  {name: "shape", type: "Rect", mode: "&", has_default: false}], returns: "Bool", builtin: false}
print(signature(C, "learn"))       // a method's (without `this`); signature(C) is the constructor's

let f = "name"
print(get_field(rex, f))           // rex
set_field(&rex, f, "max")          // checked against the field's declared type
print(call_method(rex, "speak"))   // a method by name
print(has_method(rex, "fly"))      // false

print(classes())                   // [<class Animal>, <class Dog>, ...]
let D = find_class("Dog")          // a class from its name
let fido = D("fido")
print(globals())                   // the program's global names, in order: ["Animal", "Dog", ...]
```

- A class is a value: it prints as `<class Dog>`, `type()` calls it `Class`, `c.name` is its name, and calling it makes an instance. Classes compare by identity and can be map keys. A class's own name alone, `Dog`, still makes a default instance; a variable holding a class, like `C` above, is the class itself.
- `fields`, `methods`, `parent_of` and `signature(c, "method")` take a class or an instance.
- In `signature`, a parameter with no declared type has type `"any"`, and `returns` is `"any"` when no return type was written (`"nothing"` in a `#strict` file).
- `call_method(x, name, ...)` calls a method of `x`'s class, a function stored in one of its fields, or a global function as `x.name(...)` would. A method called this way can't change `x`.

### Reactivity: `on`

An `on` block runs whenever a `var` changes, right after the change:

```lube
var score = 0
var lives = 3

on score (old, new) {                  // the value before and after
  if new > old { print("+", new - old) }
}

on score {
  print("score changed to", score)
}

on score, lives {                      // when any of these changes
  redraw()
}

on score (old, new) when new > old {   // only when the condition holds
  print("+", new - old)
}

on player.hp (old, new) {              // a field: runs only when hp itself changes
  if new <= 0 { print("game over") }
}

score += 10      // + 10, score changed to 10, redraw()...
score = 10       // the same value: nothing runs
```

`on` also works inside functions, methods and constructors, where it can watch local variables and fields of `this` as well as globals:

```lube
class Player {
  var hp = 10
  var name = "ada"
  fn fight(enemy) {
    var hits = 0
    on this.hp (old, new) when new <= 0 { print(this.name, "is down after", hits, "hits") }
    on hits { print("hit", hits) }
    while this.hp > 0 {
      this.hp -= enemy.attack()
      hits += 1
    }
  }
}
```

A block inside a function runs on that function's own frame. It reads and changes the function's variables directly, not a copy as a closure would, so a block can keep a count or update other state. It lasts as long as the block of code it's declared in: until the function returns, or until the end of the `if`, loop body or `{ }` around it. Each call gets its own blocks.

- Any change counts: an assignment (`score = 5`, `score += 1`), a change inside the value (`items.push(x)`, `player.hp -= 1`, `grid[r][c] = 1`), or a function changing it through `&` (`heal(&player)`, `player.heal()`). Assigning the value it already has is not a change.
- Blocks run in the order they were declared, after everything a call lent with `&` is back.
- A block's own changes don't run it again, so `on count { count = 0 }` can't loop. A block can change other watched vars, and their blocks run.
- An error in a block reaches the code that made the change, like an error in a function it called. A change made by a call that then fails doesn't run the blocks.
- `on` watches `var`s: globals, a function's own locals and `&` or `var` parameters, and `this` in a method. Watching a `let`, a function, a variable a closure captured (it can't change), or a variable that doesn't exist is an error. So is a field that isn't there when the block is declared. A field that's missing before or after a change is not a change.
- A block inside a function that watches a global also runs when another function changes it. If the block uses a variable the function has lent out with `&` at that moment, running it is an error.
- In a `#strict` file, `(old, new)` need types: `on score (old: Number, new: Number)`.
- With hot reload, a changed file's top-level `on` blocks replace the old ones.

### Components: `fn ... component`

A component is a function whose frame outlives its call. Its `var`s are its state, the `fn`s in its body are its handlers, its `on` blocks are its effects, and what it returns is its view. The body runs once, when the component is made. The view is worked out again each time it's asked for, from the state as it is then, and so are the component's `let`s:

```lube
fn Card(title) component {
  var open = false
  var clicks = 0
  on open (old, new) { print("open:", new) }       // an effect: runs when open changes
  on leave { print(title, "leaves") }              // runs once, at c.leave()
  fn toggle() {                                    // a handler: changes the card's own vars
    open = not open
    clicks += 1
  }
  fn add(n) => clicks += n
  let label = "{title} ({clicks})"                 // a let: worked out again with each view
  return {title: label, open: open, clicks: clicks, onclick: toggle}
}

let c = Card("Lube")
c.toggle()                 // open: true
print(c.view())            // {title: "Lube (1)", open: true, clicks: 1, onclick: <handler toggle>}
let h = c.toggle           // a handler is a function: kept, passed on, called later
h()                        // open: false
print(c.add(10))           // 12
c.leave()                  // Lube leaves
```

- A handler runs on the component's frame, as an `on` block in a function does, so it changes the component's vars themselves. A closure gets copies and can't. A handler can call the component's other handlers, wherever they are written, and the body can call them too.
- A `let` at the top of the body belongs to the view: it is worked out again, in order, before each view. So `let total = sum(prices)` is always up to date. Only the view sees it: a handler, an effect, or the body itself that uses one is an error. A value they need is a `var`, or is worked out where it is used.
- `c.view()` runs the `return` expression again. `c.name(...)` calls a handler, and `c.name` is the handler as a function. A view that holds a handler, like `onclick: toggle` above, is how a page calls back into its component: the `web` plugin serves components as pages (see Web pages below).
- `c.leave()` runs its `on leave` blocks; after that, its view and handlers are gone. Leaving twice does nothing.
- A component can hold other components in its vars, a list of them for instance, and its handlers can call theirs.
- A handler doesn't keep its component alive: once nothing else holds the component, calling the handler is an error. So a component can keep its own handlers in its vars with no cycle, and with no garbage collector nothing is left over.
- The rules:
  - A component is declared at the top level of a file.
  - Its parameters, and its handlers', are plain: no `&`, `var` or defaults. Neither has a return type.
  - Its body ends with `return` and the view, and returns nowhere else.
  - Its `on` blocks watch its own vars; a global is watched at the top level of the file.
  - A handler can't call itself, and a component can't leave while one of its blocks is running.
  - Inside a component, `view` and `leave` are its own names: a handler can't use them, and `on leave` is the leave block.
- A component isn't data, so it can't be sent to another isolate. It prints as `<component Card>`, and its type is `Component`.

### Concurrency: isolates and channels

A program can use every core. `spawn` runs a function in an *isolate*: a separate interpreter, with its own heap and globals, on its own thread. Isolates share no values (Lube's values are copies anyway), so there are no locks and no data races, and each one runs at full speed. They talk through *channels*:

```lube
fn worker(jobs: Channel, results: Channel) {
  for n in jobs {                       // until jobs is closed
    results.send(heavy(n))
  }
}

let jobs = channel()
let results = channel()
let pool = [spawn(worker, jobs, results), spawn(worker, jobs, results)]
for n in 0..100 { jobs.send(n) }
jobs.close()
for w in pool { w.join() }
print(sum(results.take(100)))

let squares = parallel_map(list(1..=1000), square)   // f on every core, results in order
print(spawn(add, 2, 3).join())                       // 5: join() gives the function's result
```

- `spawn(f, args...)` starts the top-level function `f` (declared with `fn` at the top of a file) and returns an `Isolate`. `iso.join()` waits for it and gives a copy of its result; if `f` failed, `join()` raises its error. An error nobody joins is reported when the isolate ends.
- `channel()` is a queue; `channel(n)` holds at most `n` values, so `send` waits while it is full. `ch.send(v)` copies `v` in; `ch.receive()` waits for a value; `ch.try_receive(default)` never waits; `ch.take(n)` gives `n` values as a list; `ch.close()` ends it, and `for v in ch` reads until then.
- What can be sent: numbers, text, bytes, lists, maps and objects, class instances, ranges, and channels themselves (that is how isolates find each other). Functions and open files can't, and sending deep values copies them.
- A new isolate runs the program's declarations: its functions, classes, imports and `on` blocks. A global variable gets its value the first time code in the isolate uses it, so a global that starts isolates or reads a big file costs nothing in a worker that doesn't touch it.
- **Deadlocks are errors, not hangs.** When an isolate would wait (on a channel, or a join) while every other isolate is already waiting, nothing could ever wake it, so it gets *deadlock: every isolate is waiting* instead. A forgotten `close()` shows up at once.
- The program ends when the main program and every isolate it started have finished. An isolate still waiting then gets the deadlock error.
- `cores()` tells how many cores the machine has. `Channel` and `Isolate` are types like any other (`fn worker(jobs: Channel)`), and `Channel` alone makes a new channel.

On a 10-core machine, `parallel_map` over 40 heavy calls runs 4.8× faster than `map`.

### Bytes and time

`Bytes` holds raw bytes: a save file, an asset, a network packet. It is a value like a list: `b[i]` is a byte (a whole Number from 0 to 255), it changes in place, and a copy is a separate value (copied only when one of them changes).

```lube
var b = Bytes("hi")             // also Bytes(n) (n zeros), Bytes([1, 2, 3]), Bytes() or just Bytes
b[0] = 72
push(&b, 33, "!?")              // a byte, a str's bytes, or more Bytes
print(b, len(b), b.decode(), b.hex())   // Bytes(48 69 21 21 3f) 5 Hi!!? 486921213f
print(b + Bytes([0]), b.slice(1, 3), 33 in b, "!?" in b)
for x in b { print(x) }
```

Binary formats use `pack` and `unpack`, with a format: `u8`, `i8`, `u16`, `i16`, `u32`, `i32`, `u64`, `i64`, `f32` or `f64`. They are little-endian, the order of every common CPU; add `be` for big-endian (`u32be`), the order of network protocols. `u64` and `i64` read as `uint64` and `int64`, so no bit is lost.

```lube
var packet = Bytes()
packet.pack("u16be", 0)                // room for its length, filled in below
packet.pack("u8", 7)                   // each one added at the end
packet.pack("f32", player.x)
packet.pack("u16be", len(packet), 0)   // written at a position, over what is there (past the end, it grows)
let x = packet.unpack("f32", 3)        // the number at byte 3
```

For a game loop, `now()` is a steady clock in seconds (to the microsecond), and `sleep(seconds)` waits:

```lube
var last = now()
while running {
  let t = now()
  update(t - last)
  last = t
  sleep(max(0, 1 / 60 - (now() - t)))
}
```

### Money: `m`

```lube
let price = m(19.99)                   // $19.99: in the default currency, USD until currency() sets another
print(m(0.1) + m(0.2) == m(0.3))       // true: exact, as 0.1 + 0.2 == 0.3 is not
let tax = price * 0.0725               // $1.45: a Number joins in, and the result rounds half up to the cent
print(price + tax, (price + tax) * 3)  // $21.44 $64.32
print(m(100).split(3))                 // [$33.34, $33.33, $33.33]: shares that add up to the cent
currency("PHP")                        // or currency("₱"): m() without a currency is in pesos now
print(m(1500), m(5, "EUR"), m(1234.5, "JPY"))   // ₱1,500.00 €5.00 ¥1,235
print(m(10, "USD").to("PHP", 58.25))   // ₱582.50
```

`m(amount, currency?)` (or `Money(...)`) makes an amount of money: a count of the currency's smallest unit (cents, yen, fils), held exactly in 64 bits, so sums never drift as floating point's do. The amount is a Number, a sized integer, or text (`m("19.99")`, `m("1,234.50")`). A Number counts as the decimal it is written as: `m(2.675)` is `$2.68`, though the double nearest 2.675 is a little less. The currency is an ISO code (`"EUR"`, any case) or a symbol (`"€"`, `"₱"`, `"R$"`); without one it is the default, which `currency("PHP")` sets and `currency()` gives (each isolate starts with its spawner's). `Money` alone is zero in the default currency.

- Money adds to and subtracts money of its currency, or a Number (in that currency): `price + 1`. It multiplies and divides by Numbers, rounding half up (away from zero) to the smallest unit; money divided by money of its currency is a Number (`m(10) / m(4)` is 2.5), and `%` works as for Numbers. It compares with money of its currency and with Numbers. Money of two currencies doesn't mix: `m(1) + m(1, "EUR")` is an error, and `to("EUR", 0.92)` converts at a rate.
- It prints as `$1,234.50`, `-€0.25`, `¥1,235`, `CHF 5.00`, in text and in interpolation; `format({symbol, code, thousands, decimal})` prints it another way (`{code: true}` gives `USD 1,234.50`, `{thousands: ".", decimal: ","}` gives `$1.234,50`, `{symbol: false}` leaves it out).
- `split(n)` gives n shares as equal as can be, and `allocate(ratios)` shares in proportion (`m(100).allocate([1, 2])` is `[$33.33, $66.67]`); both add up exactly to the amount, the leftover units going to the shares that lost most to rounding. `round(decimals?)` rounds to whole units (or that many decimals), half up.
- Properties: `amount` (a Number), `units` (in the smallest unit: `m(19.99).units` is 1999), `currency` (its code), `symbol` and `decimals`. `abs`, `sum`, `min`, `max` and `sort` take money, and `Number(x)` gives its amount.
- The currencies are ISO 4217's, with their decimals (JPY and KRW have none, BHD, KWD and JOD three). `add_currency("BTC", "₿", 8)` adds one of one's own: a code, a symbol and its decimals.
- An amount holds up to 2⁶³ of its smallest unit (92 quadrillion dollars); a result past that is an error, as is money that is nan or infinite.

### Plugins: files, sockets, sound, graphics, maths, machine learning and web pages

Some of Lube comes as plugins: packages written in C, loaded when a program imports them, so a program that uses none of them doesn't load them. `make` builds those that come with Lube into `plugins/` next to `lube`: `fs` for files and folders, `net` for sockets, `audio` for sound and `gfx` for graphics; `math`, `cmath`, `statistics` and `random`, Python's modules of those names; `array`, `nn`, `learn` and `llm`, which do what numpy, PyTorch, scikit-learn and llama.cpp do; and `web` for web pages. They are imported as any package is:

```lube
import fs.*                // open, exists, list_dir...: their names, bare
import net                 // net.tcp_listen(7777), net.Socket
import audio as sfx        // sfx.Sound("jump.wav")
import gfx.*               // window(320, 180), image(16, 16), rgb(255, 128, 0)...
import math.*              // sqrt, gamma, fsum, comb... as Python's math
import array as np         // np.zeros(3, 4), np.solve(a, b): Arrays, as numpy's
```

A plugin's types (`File`, `Socket`, `Sound`, `Voice`, `Image`, `Window`, `Complex`, `Array`, `Tensor`...) work as classes do: `x is Socket`, `fn send_all(s: Socket)`, `class_of(x)`, `methods(x)`. Their objects are the plugin's: a copy shares the one file or socket, and they stay in the isolate that made them. Anyone can write a plugin: see Writing a plugin below.

### Files and folders: `fs`

```lube
import fs.*                     // or import fs: fs.open(...), fs.File
```

`open(path, mode)` gives a `File`. The mode is `"r"` (read, the default), `"w"` (write, emptied first), `"a"` (add at the end) or `"r+"`, `"w+"`, `"a+"` (read and write). Files are always read and written as bytes, with no translation of line endings.

```lube
let f = open("level1.dat")
let header = f.read(16)         // up to 16 bytes, as Bytes; f.read() reads the rest
let name = f.read_line()        // a line as a str, without "\n" or "\r\n"; an error at the end
f.seek(-4, "end")               // from "start" (the default), "current" or "end"; f.tell() says where
print(f.size(), f.eof())
f.close()                       // or let it go: a file closes when nothing holds it

let log = open("log.txt", "a")
log.write("started\n")          // a str or Bytes; returns how many bytes were written
log.flush()
```

Copies of a `File` share one open file; `close()` closes it for all of them, and using it after that is an error. Whole files: `read_bytes(path)` and `write_bytes(path, bytes)`, or `read_file`, `write_file` and `append_file` for text.

Folders: `exists(path)`, `is_dir(path)`, `list_dir(path)` (the names in it, sorted), `make_dir(path)` (with the folders above it, and nothing to do if it is there), `remove_file(path)` (a file or an empty folder), `rename_file(from, to)` (replacing `to`) and `file_size(path)`. An error says what went wrong: *cannot open 'save.bin': No such file or directory*.

### Sockets: `net`


TCP for connections and UDP for packets, made for game loops (`import net.*`, or `import net` for `net.tcp_listen(...)`). Nothing waits unless asked to: `accept()`, `receive()` and `packets()` give what has arrived, and an empty list or `Bytes` when nothing has. Each takes the seconds to wait for something first, and `poll(sockets, seconds)` waits until any of them has something to read, so a server sleeps between frames instead of spinning:

```lube
import net.*
let server = tcp_listen(7777)            // every address of this machine; tcp_listen(7777, "127.0.0.1") for just this one
var clients = []
while true {
  for c in server.accept() { push(&clients, c) }
  for c in clients {
    let msg = c.receive()                // Bytes: what has arrived, maybe part of a message, maybe nothing
    if len(msg) > 0 { c.send(msg) }      // send takes Bytes or a str
  }
  clients = clients.filter(fn(c) => c.is_open())   // closed by the other side: is_open() is false
  poll(clients + [server], 1 / 60)       // wait for the next message or the next frame
}
```

```lube
let c = tcp_connect("example.com", 80)   // waits up to 10 seconds; tcp_connect(host, port, seconds)
c.send("GET / HTTP/1.0\r\nHost: example.com\r\n\r\n")
var page = Bytes()
while c.is_open() { page += c.receive(INF) }   // INF: wait as long as it takes
print(page.decode())
```

- TCP is a stream: one `send` can arrive as several `receive`s, and several as one. Put the length first (`b.pack("u16be", len(msg))`) and collect `Bytes` until a whole message is there.
- `c.send(data)` sends all of it and returns `true`, or `false` once the other side has closed the connection; then `c.receive()` is empty and `c.is_open()` is `false`. Connections send at once (no Nagle delay), as games want.
- UDP: `udp_open(port?, host?)` (port 0, the default, lets the system choose), `u.send_to(host, port, data)`, and `u.packets(seconds?)`, every packet that has arrived as `{data, host, port}`: reply with `u.send_to(p.host, p.port, ...)`. `u.receive()` gives just the next packet's bytes. Packets can be lost or come out of order, and should stay under about 1200 bytes.
- `s.port()` is the socket's own port (useful after port 0), `c.peer()` the other side of a connection (`"203.0.113.9:52431"`), `s.close()` closes it, and a socket also closes when nothing holds it.
- An isolate can wait as long as it likes (`receive(INF)`, `accept(INF)`) without holding up the others. A socket stays in the isolate that made it: it cannot be sent.
- `tcp_connect` tries a host's IPv4 addresses first, then IPv6; to listen on IPv6, give the host `"::"`. Errors say what happened: *tcp_connect(): cannot connect to 127.0.0.1 port 7777: Connection refused*.

### Sound: `audio`


A `Sound` holds samples: from a WAV file, or made by the program (`import audio.*`, or `import audio` for `audio.Sound(...)`). Playing one gives a `Voice`, and any number of voices (up to 64 at once) are mixed on a thread of their own, so the game loop never waits for sound.

```lube
import audio.*
let jump = Sound("jump.wav")            // or Sound(bytes): a WAV file read some other way
let music = Sound("theme.wav").loop(0.6)   // loops until stopped; 0.6 is its volume
jump.play()                             // fire and forget: it plays on, held or not
let engine = Sound("engine.wav").loop()
engine.set_speed(1 + speed / 100)       // faster is higher, as on tape
engine.set_pan(-0.5)                    // -1 left, 0 both sides, 1 right
engine.set_volume(0.3)                  // changes take a few milliseconds: no clicks
music.stop()                            // fades out at once
```

Sounds can be made from numbers, from -1 to 1, 44100 a second unless told otherwise:

```lube
var samples = Number[]
for i in 0..4410 { push(&samples, sin(2 * PI * 880 * i / 44100) * (1 - i / 4410)) }  // a falling ping
let ping = Sound(samples)               // Sound(samples, rate, 2) for stereo: left, right, left, ...
ping.play(0.5, 1)                       // volume 0.5, on the right
write_bytes("ping.wav", ping.to_wav())  // a 16-bit WAV file of it
```

- `Sound(...)` reads WAV files of 8, 16, 24 or 32-bit PCM or 32 or 64-bit floats, mono or stereo, at any rate (each voice is resampled as it plays). `s.duration()` (seconds), `s.rate()`, `s.channels()` and `s.samples()` describe it; `Sound()` is silence.
- `s.play(volume?, pan?)` and `s.loop(volume?, pan?)` give a `Voice`: `v.stop()`, `v.is_playing()`, `v.set_volume(x)`, `v.set_pan(x)` and `v.set_speed(x)` (0 holds it where it is). A volume goes from 0 to 1 (as recorded), or up to 16 for louder. With 64 playing, a new voice takes the place of the oldest that would end on its own.
- `stop_all()` stops them all, `set_volume(x)` sets the volume of everything, and `device()` says what sound comes out of.
- Sound needs no library to build Lube: it uses what the system has, loaded when the first sound plays (so a program that plays none never touches it). That is Audio Queue on macOS, ALSA on Linux and the Raspberry Pi, and waveOut on Windows. Without one (a server, no sound card, or `LUBE_AUDIO=none`), sounds play in silence, taking their time and ending as they would. The static Linux binary from the releases is always silent (its plugins are inside it, and a static program cannot load ALSA): build Lube with `make` to have sound there.
- When the program ends, its sounds stop: a script that only plays a sound should wait for it with `sleep(s.duration())`. Sounds and voices belong to the isolate that made them, but any isolate can play sounds.

### Graphics: `gfx`

2D graphics drawn by Lube itself, pixel by pixel: shapes, images (scaled, flipped, turned, faded), text in a built-in pixel font, and PNG files read and written. An `Image` is pixels in memory, so a server with no screen can draw too. A `Window` is an image the screen shows: on a computer, in a window of its own; on a phone, filling the screen (see Phones below).

```lube
import gfx.*
let win = window(320, 180, "Space", 3)               // 320 by 180 pixels, each shown as 3 by 3
let ship = load_image("ship.png")                    // or load_image(bytes)
var x = 160
while win.is_open() {
  for e in win.events() {                            // what happened since the last frame
    if e.kind == "key_down" and e.key == "escape" { win.close() }
    if e.kind == "mouse_down" { x = e.x }            // a click, or a finger on a phone
  }
  if win.key_down("left") { x -= 2 }                 // held down now
  win.clear()                                        // black
  win.draw(ship, x, 80, {rotate: 90, scale: 2})
  win.fill_circle(40, 40, 10, color("orange"))
  win.text("score: 10", 4, 4, rgb(255, 255, 0))
  win.present()                                      // shows the frame, then waits for the next (60 a second)
}
```

- Colors are Numbers, `0xAARRGGBB`: `rgb(255, 128, 0)`, `rgba(255, 128, 0, 128)` (half transparent), or `color("orange")` (black, white, red, green, blue, yellow, cyan, magenta, gray, orange, purple, brown, pink, transparent) and `color("#ff8000")` (`#rgb`, `#rrggbb` or `#rrggbbaa`). What is drawn goes over what is there, as its transparency says.
- `image(w, h, color?)` makes an image (transparent, unless given a color). Images and windows draw the same way: `clear(color?)`, `pixel(x, y)` (0 outside the edges), `set_pixel(x, y, color)`, `line`, `rect` and `fill_rect(x, y, w, h, color)`, `circle` and `fill_circle(x, y, r, color)`, `fill_triangle(x1, y1, x2, y2, x3, y3, color)`, and `text(s, x, y, color?, scale?)` (white, 1). What falls outside the edges is left out.
- `draw(img, x, y, options?)` draws an image (or a window) with its top left at `x, y`. The options: `part: [x, y, w, h]` (a sprite of a sheet), `scale`, `scale_x`, `scale_y`, `flip_x`, `flip_y`, `rotate` (degrees, clockwise, around its middle) and `alpha` (0 to 1).
- `copy()`, `crop(x, y, w, h)`, `width()` and `height()`; `to_png()` gives the PNG file's `Bytes`, and `save_png(path)` writes it. `load_image` reads every kind of PNG (gray, RGB, palette, with alpha, 1 to 16 bits, interlaced), and says what is wrong with one it can't. `text_size(s, scale?)` is `[width, height]` in pixels (6 by 10 a letter).
- One window at a time: `window(w, h, title?, scale?)`. `win.events()` is a list of `{kind, ...}`: `key_down` (`key`, `repeat`), `key_up`, `text` (`text`, what was typed), `mouse_move`, `mouse_down` and `mouse_up` (`x`, `y` in the window's pixels, `button`: 1 left, 2 right, 3 middle), `wheel` (`dx`, `dy`), `touch_down`, `touch_move` and `touch_up` (`id`, `x`, `y`: on a phone the first finger works the mouse too), `focus`, `blur` and `close` (the close button: the window closes). `win.key_down(name)` and `win.mouse()` (`{x, y, left, right, middle}`) say what is held now, and `keys()` lists the keys' names.
- `win.present()` keeps to 60 frames a second (`win.set_fps(n)`; 0 for no waiting). `win.set_title(s)`, `win.close()`, `win.is_open()`. A window is drawn by the program, so it stays sharp at any scale: the screen shows each pixel as a square of whole pixels where it can.
- `win.set_fullscreen(true)` fills the screen with it (its pixels as large as fit, black around them), and `false` makes it a window again; `win.is_fullscreen()` says which (on macOS, the window's green button changes it too). `win.show_cursor(false)` hides the mouse's arrow over the window.

Game controllers are numbered from 0, in the order they connect, and their buttons are named by where they are, as on an Xbox controller: `a` at the bottom, `b` on the right, `x` on the left, `y` at the top (on a PlayStation controller, cross, circle, square and triangle):

```lube
for e in win.events() {
  if e.kind == "pad_connected" { print("controller", e.pad, "is", e.name) }
  if e.kind == "pad_down" and e.button == "a" { jump() }
}
let p = win.pad(0)                                   // controller 0 now
x += p.left_x * speed                                // a stick: -1 to 1 (right and down are more)
if p.right_trigger > 0.5 { fire() }                  // a trigger: 0 to 1
```

- Events: `pad_connected` (`pad`, `name`), `pad_disconnected` (`pad`), `pad_down` and `pad_up` (`pad`, `button`). Controllers already connected when the window opens come as `pad_connected` too, in its first events.
- `win.pads()` lists the numbers of those connected. `win.pad(n)` is `{connected, name}`, a Bool for each button (`a`, `b`, `x`, `y`, `left_shoulder`, `right_shoulder`, `back`, `start`, `guide`, `left_stick`, `right_stick`, `up`, `down`, `left`, `right`), and the axes as Numbers: `left_x`, `left_y`, `right_x`, `right_y` (with the middle fifth of each stick, where a stick at rest is never quite still, as 0) and `left_trigger`, `right_trigger`. A trigger pressed past half way is also a button, `left_trigger` or `right_trigger`, for `pad_down`. `pad_buttons()` lists the names.
- They come from what each system has: XInput on Windows (Xbox controllers, and the many that work as one), GameController on macOS and iOS (Xbox, PlayStation, Switch and MFi controllers), the kernel's own on Linux (the controllers it knows: Xbox, PlayStation, Switch, Steam and most others), and Android's.
- Windows need no library to build Lube: they use what the system has, loaded when the first window opens. That is Cocoa on macOS, X11 on Linux and the Raspberry Pi, and Win32 on Windows. With `LUBE_GFX=none` a window is shown nowhere (on a server, or in tests: everything works but the screen), and the static Linux binary from the releases has no windows (it can still draw images and save them).

### Maths: `math` and `cmath`

`math` is Python's math module, all of it, and `cmath` is Python's cmath module, with complex numbers (`import math.*`, or `import math` for `math.sqrt(2)`). Their answers are Python's: the same algorithms, and the same last bit wherever the C library's `exp`, `log`, `sin`... allow.

```lube
import math.*
print(sqrt(2), hypot(3, 4), log(8, 2), degrees(pi))   // 1.4142135623730951 5 3 180
print(fsum([0.1, 0.2, 0.3]), sum([0.1, 0.2, 0.3]))    // 0.6 0.6000000000000001: fsum rounds once, exactly
print(factorial(15), comb(52, 5), gcd(12, 18, 27))    // 1307674368000 2598960 3
print(isclose(1, 1.0000001, {rel_tol: 1e-6}))         // true: Python's keyword arguments come in an object
sqrt(-1)                                              // error: sqrt() of a negative number (-1): cmath.sqrt gives complex roots
```

```lube
import cmath.*
let z = Complex(3, 4)                                 // or Complex("3+4j")
print(z, abs(z), z.conjugate(), z * 2 + 1)            // (3+4j) 5 (3-4j) (7+8j)
print(sqrt(-4), log(-1), exp(Complex(0, pi)))         // 2j 3.141592653589793j (-1+1.2246467991473532e-16j)
print(polar(Complex(0, 2)), rect(2, 0))               // [2, 1.5707963267948966] (2+0j)
```

- `math` has every name of Python 3.15's: `sqrt`, `cbrt`, `exp`, `exp2`, `expm1`, `log(x, base?)`, `log2`, `log10`, `log1p`, `pow`; `sin`, `cos`, `tan`, `asin`, `acos`, `atan`, `atan2`, `sinh`, `cosh`, `tanh`, `asinh`, `acosh`, `atanh`, `degrees`, `radians`, `hypot(x, y, ...)`, `dist(p, q)`; `floor`, `ceil`, `trunc`, `modf`, `frexp`, `ldexp`, `fmod`, `remainder`, `copysign`, `fabs`, `fma`, `fmax`, `fmin`, `nextafter`, `ulp`; `isfinite`, `isinf`, `isnan`, `isnormal`, `issubnormal`, `signbit`, `isclose`; `fsum`, `prod`, `sumprod`; `factorial`, `comb`, `perm`, `gcd`, `lcm`, `isqrt`; `gamma`, `lgamma`, `erf`, `erfc`; and `pi`, `e`, `tau`, `inf`, `nan`. (Python's `math.integer` is not there: its functions are math's own.)
- Where Python raises an error, so does `math`: `sqrt(-1)`, `log(0)`, `acos(2)`, `exp(1000)` (*exp(1000): the result is too large for a Number*), `factorial(1.5)`. Lube's own `sqrt`, `log`, `pow`... give nan or inf there instead; `import math.*` replaces them. `floor`, `ceil` and `trunc` give Numbers, and keep inf, nan and sized integers as they are, as Lube's own `floor` does.
- Python's keyword arguments come as an object at the end: `isclose(a, b, {rel_tol: 1e-6, abs_tol: 0})`, `prod(xs, {start: 2})`, `nextafter(x, y, {steps: 3})`. Its sequences are Lists (`fsum`, `prod`, `sumprod`, `dist`), and `gcd`, `lcm` and `hypot` take any number of arguments.
- `factorial`, `comb`, `perm`, `gcd`, `isqrt`, and `prod` of whole numbers, work out the exact integer, as Python does, and give the Number nearest it: exact up to 2⁵³, and right to 17 digits beyond (`comb(1000, 500)` is `2.7028824094543655e+299`). An answer past the largest Number, such as `factorial(171)`, is an error. `isqrt` is exact below 2¹⁰⁶ and `lcm` below 2⁵³.
- `fsum` is exactly rounded, `hypot` and `dist` nearly always are, `sumprod` adds in extended precision, and `fma(x, y, z)` rounds once: these give the same last bit on every system. What comes from the C library (`exp`, `log`, `sin`, `erf`, and `gamma` through `pow`) may differ in its last bit from one system to another, as Python's does.
- `cmath`'s `Complex(re, im?)` makes a complex number; `Complex("1+2j")` reads Python's forms (`"2j"`, `"(1-2.5e3J)"`, `"inf+nanj"`). It has `z.real`, `z.imag`, `z.conjugate()`, the operators `+ - * / **`, `==`, `!=` and `-z`, with Numbers on either side, and prints as Python prints a complex: `(1+2j)`, `2j`, `(1-0j)`. A Number with a Complex is a real number, as in Python: `2 * Complex(inf, 1)` is `(inf+2j)`, not nan. `Complex(1) / 0` is an error, as is `%` or `<` (complex numbers have no order).
- `cmath`'s `sqrt`, `exp`, `log(z, base?)`, `log10`, `sin`, `cos`, `tan`, `asin`, `acos`, `atan`, `sinh`, `cosh`, `tanh`, `asinh`, `acosh` and `atanh` take a Number or a Complex and give a Complex, with Python's branch cuts and its answers for infinities: `sqrt(-4)` is `2j`. Then `phase(z)`, `polar(z)` (`[r, phi]`), `rect(r, phi)`, `abs(z)` (which takes Numbers too, as the `abs` it replaces), `isfinite`, `isinf`, `isnan` and `isclose`; `pi`, `e`, `tau`, `inf` and `nan`; and `infj()` and `nanj()`, functions (a package's constants are Numbers).
- The two share names (`sqrt`, `log`, `pi`...): with both, import one with `.*` and the other by name (`import math.*` and `import cmath`, then `cmath.sqrt(-1)`); with both as `.*`, the names of the one imported last win.

### Statistics and random numbers: `statistics` and `random`

Python's `statistics` and `random` modules, as two plugins that give Python's answers. The same seed gives the same random numbers as in Python, digit for digit, and the statistics come out as Python's do: `mean`, `harmonic_mean`, the variances and the standard deviations are worked out exactly (Python uses fractions for them) and rounded once, so even data that would trip up a running total gets the right last digit.

```lube
import random
random.seed(42)
print(random.random())                  // 0.6394267984578837, as in Python
print(random.randint(1, 6), random.choice(["rock", "paper", "scissors"]))
let deck = random.shuffle(cards)        // a new list: cards stays as it was
let hand = random.sample(deck, 5)       // 5 cards, none twice
let rolls = random.choices([1, 2, 3, 4, 5, 6], {weights: [1, 1, 1, 1, 1, 5], k: 10})
let r = random.Random(7)                // a generator of its own: r.random(), r.gauss(0, 1)...
```

```lube
import statistics.*
let times = [2.75, 1.75, 1.25, 0.25, 0.5, 1.25, 3.5]
print(mean(times), median(times), stdev(times))      // 1.6071428571428572 1.25 1.171334200612195
print(quantiles(times, {n: 4, method: "inclusive"}))  // [0.875, 1.25, 2.25]
print(correlation(xs, ys), linear_regression(xs, ys).slope)
let iq = NormalDist(100, 15)
print(iq.cdf(130), iq + 5)        // 0.9772498680518208 NormalDist(mu=105, sigma=15): it works with + - * / ==
let f = kde(times, 0.5)                              // f.pdf(x), f.cdf(x)
```

- Python's keyword arguments come in an object at the end: `quantiles(data, {n: 10})`, `correlation(x, y, {method: "ranked"})`, `linear_regression(x, y, {proportional: true})`, `choices(xs, {cum_weights: cw, k: 3})`, `sample(xs, 5, {counts: [4, 2]})`, `NormalDist().samples(10, {seed: 1})`. Positional arguments stay positional: `fmean(data, weights)`, `variance(data, xbar)`, `choices(xs, weights, {k: 5})`.
- `random`'s functions share one generator, under a lock, so isolates can call them at once; a `Random(seed?)` has one of its own (its methods are the same functions), and `SystemRandom()` draws the system's random bytes (it has no seed and no state). With `import random`, the name `random` is the package, so Lube's own `random()` is `random.random()` in that file, as in Python.
- A seed is a whole Number (seeding as Python's int does: `seed(42)`), another Number (as Python's float), a str or `Bytes` (through SHA-512, as Python does; `seed(text, 1)` for Python 2's way), or nothing: `seed()` takes the system's randomness.
- What differs from Python: `shuffle(xs)` gives a new list in the order Python's `shuffle` would leave `xs` (a plugin can't change a list in place); `getrandbits(k)` takes `k` up to 53, the bits a Number holds whole (`randbytes(n)` gives more); `getstate()` is a List, `[3, [625 Numbers], gauss]`, with `false` where Python has `None`, and `setstate` takes it back (or Python's, written as a list). `gammavariate`, `betavariate` and `vonmisesvariate` refuse a NaN or infinite shape, which makes Python's loop forever.
- Python's `kde` returns a function and `kde_random` a function to call; here `kde(data, h, kernel?)` gives a `Kde` with `k.pdf(x)` and `k.cdf(x)` (Python's `cumulative=True` is `k.cdf`), and `kde_random(data, h, kernel?, {seed})` a `KdeRandom` with `r.sample()` and `r.samples(n)`. The kernels are Python's: `"normal"` (or `"gauss"`), `"logistic"`, `"sigmoid"`, `"rectangular"` (`"uniform"`), `"triangular"`, `"parabolic"` (`"epanechnikov"`), `"quartic"` (`"biweight"`), `"triweight"` and `"cosine"`.
- `NormalDist.from_samples(data)` is `from_samples(data)`. A `NormalDist` has the properties `mean`, `median`, `mode`, `stdev` and `variance`, the methods `pdf`, `cdf`, `inv_cdf`, `overlap`, `quantiles`, `samples` and `zscore`, and works with `+` and `-` (a Number or another `NormalDist`), `*` and `/` (a Number), `-d` and `==`. Its draws, and `kde_random`'s, come from `random`'s generator, so `random.seed(1)` and a seed option give Python's numbers.
- `StatisticsError` is a Lube error that says what is wrong: *variance() needs at least two data points*, *correlation(): at least one of the inputs is constant*. `mode` and `multimode` count Numbers, text and Bools.

### Arrays: `array`

```lube
import array as np                     // (or import array.*: its names bare)
let a = np.array([[1, 2, 3], [4, 5, 6]])
print(a.shape, a.dtype, a.T.shape)     // [2, 3] float64 [3, 2]
print(a * 2 + 1, a @ a.T)              // elementwise, broadcasting; @ is the matrix product
print(a[0, 1], a[.., 0], a[1, 1..])    // 2 [1, 4] [5, 6]
print(a[a > 2], a.sum(0), a.mean())    // [3, 4, 5, 6] [5, 7, 9] 3.5
print(np.solve(np.array([[4, 1], [2, 3]]), [1, 2]))   // [0.1, 0.6]
np.seed(0)
print(np.rand(3))                      // [0.5488135039273248, 0.7151893663724195, 0.6027633760716439], as numpy's
```

An `Array` is n-dimensional (up to 16) and of one dtype: `"float64"` unless asked, `"float32"`, `"int64"`, `"int32"`, `"uint8"` or `"bool"`. Lube's Numbers don't split into ints and floats, so `array([1, 2])` is float64 where numpy's would be int64; what gives positions (`argmax`, `argsort`, `nonzero`, `randint`, `bincount`) gives int64.

- Making them: `array(x, dtype?)` (Numbers, Bools, nested lists, ranges, Bytes, other Arrays), `asarray`, `zeros(2, 3)` or `zeros([2, 3])` (a dtype after the shape: `zeros(2, 3, "float32")`), `ones`, `empty`, `full(shape, value)`, `zeros_like`, `ones_like`, `full_like`, `arange(start?, stop, step?)`, `linspace(a, b, n?, {endpoint})`, `logspace`, `eye(n, m?, k?)`, `identity`, `diag`, `from_bytes(b, dtype?, shape?)`; and joined: `concatenate(xs, axis?)`, `stack`, `vstack`, `hstack`, `tile`, `repeat`.
- The operators are elementwise and broadcast as numpy's: `+ - * / % **`, `-a`, and the comparisons, which give Arrays of Bools: `if a > 0` is an error (*a condition needs a Bool*): ask `any()` or `all()`, or `array_equal(a, b)`. `@` is `matmul`. `len(a)` is the length of the first axis, and `for row in a` goes over it.
- Indexing: `a[i]`, `a[i, j]`, negative from the end; ranges are slices (`a[1..]`, `a[..3]`, `a[.., 0]`, `a[range(0, 10, 2)]`), lists or Arrays of positions pick those (`a[[0, 2]]`, `a[[0, 1], [2, 0]]`), and an Array of Bools picks where it is true (`a[a > 0]`). Numbers and slices give a view, in no time; the others give a copy. All of them can be assigned to: `a[a < 0] = 0`, `a[.., 0] = [7, 8]`.
- Arrays are values, as lists are: after `var b = a`, `b[0, 0] = 100` leaves `a` as it was. They share their memory until one of them changes, so views and copies cost nothing until then; `a += 1` changes `a` in place when nothing else holds it.
- Properties `shape`, `ndim`, `size`, `dtype`, `T`, `itemsize`, `nbytes`; methods `reshape(...)` (one axis may be `-1`), `flatten`, `ravel`, `transpose(axes?)`, `swapaxes`, `squeeze`, `astype(dtype)`, `copy`, `to_list`, `to_bytes`, `item(i?)`, `dot`, `clip`, `round`, `sort`, `argsort`, `nonzero`, and the reductions.
- Elementwise functions, which take Numbers too (so `import array.*` can replace Lube's own `sqrt`, `abs`...): `abs`, `sqrt`, `cbrt`, `exp`, `exp2`, `expm1`, `log(x, base?)`, `log2`, `log10`, `log1p`, `sin`, `cos`, `tan`, `arcsin`, `arccos`, `arctan`, `sinh`, `cosh`, `tanh`, `arcsinh`, `arccosh`, `arctanh`, `erf`, `floor`, `ceil`, `trunc`, `rint`, `round(x, decimals?)` (half to even, as numpy's and Python's), `sign`, `square`, `reciprocal`, `negative`, `isnan`, `isinf`, `isfinite`, `logical_not`; and of two: `add`, `subtract`, `multiply`, `divide`, `mod`, `power`, `maximum`, `minimum`, `arctan2`, `hypot`, `fmod`, `floor_divide`, `copysign`, the comparisons by name (`less`, `equal`...), `logical_and`, `logical_or`, `logical_xor`, `where(cond, x, y)`, `clip`, `isclose`, `allclose`.
- Reductions over everything or along an axis (`a.sum(0)`, `{keepdims: true}`): `sum`, `prod`, `mean`, `var` and `std` (`{ddof: 1}`), `min`, `max`, `argmin`, `argmax`, `any`, `all`, `ptp`, `cumsum`, `cumprod`, `count_nonzero`, `average(a, axis?, {weights})`, `median`, `percentile(a, q, axis?)`, `quantile`. `min` and `max` of several Numbers are still their smallest and largest. Then `sort` and `argsort` (along an axis; argsort keeps equal ones in order), `unique(a, {return_index, return_inverse, return_counts})`, `searchsorted`, `bincount`, `nonzero`, `flip`, `roll`.
- Linear algebra: `matmul`, `dot`, `inner`, `outer`, `trace`, `det`, `slogdet`, `inv`, `solve`, `lstsq` (`[x, residuals, rank, s]`), `pinv`, `matrix_rank`, `qr` (`[q, r]`), `svd` (`[u, s, vh]`, `{full_matrices, compute_uv}`), `eigh` and `eigvalsh` (symmetric matrices), `cholesky`, `norm(x, ord?, axis?)`. Decompositions work in float64 and give float32 for float32. They call the system's LAPACK where there is one, as numpy does (Accelerate's on macOS, the one inside OpenBLAS, or `liblapack`), on the matrices by columns as numpy passes them, so they give numpy's answers to the last digit when both use the same LAPACK; `use_blas(false)` turns to array's own code (Householder tridiagonalization and the QL method for `eigh`, one-sided Jacobi for `svd`), which gives the same answers on every system.
- Random Arrays give numpy's numbers for the same seed (its `RandomState`): `seed(n?)`, `rand(shape...)`, `randn`, `random(size?)`, `uniform(low, high, size?)`, `normal(loc, scale, size?)`, `standard_normal`, `exponential`, `randint(low, high?, size?)`, `choice(a, size?, {replace, p})`, `permutation(x)`, and `shuffle(x)` (a new Array). One generator for the process, under a lock.
- Files: `save(path, a)` and `load(path)` write and read numpy's `.npy`, both ways; `loadtxt(path, {delimiter, skiprows, dtype})` and `savetxt(path, a, {fmt, delimiter, header})` numbers in columns; `load_safetensors(path)` and `save_safetensors(path, {name: a})` the format models come in (float16 and bfloat16 become float32).
- Speed: the matrix product and the decompositions use the system's BLAS and LAPACK where there is one (Accelerate on macOS, OpenBLAS on Linux or Windows when it is installed: `blas()` says which, and `use_blas(false)` turns to array's own kernel, blocked and packed), and loops over big Arrays share out among threads (`threads(n)`; their results don't depend on how many). `median`, `percentile` and `quantile` select the elements they need (Floyd and Rivest's way) instead of sorting; a bool mask of the Array's own shape is counted and copied in one go; reductions along an axis that is not the last go row by row, so that they read memory in order. Against numpy 2.5 on an M5: `exp` of 10 million numbers 5 times as fast, `median` of a million twice as fast and along an axis 5 times, `x[x > 0]` 4 to 19 times, `unique` up to 6 times, sorting a million 1.2 times, small Arrays' operations about twice as fast (Lube's calls cost less), and big matrix products and decompositions the same (both are Accelerate's). `bench/vs_python` compares every function at several sizes.
- What numpy has and this hasn't: complex Arrays (see `cmath` for complex numbers), the smaller dtypes (int8, int16, float16), structured dtypes, `np.linalg.eig` for matrices that aren't symmetric, and numpy's keyword arguments, which come as an object at the end.

### Neural networks: `nn`

```lube
import nn
let x = nn.tensor([2, 3], {requires_grad: true})
let y = (x * x).sum()
y.backward()
print(y.item(), x.grad)                 // 13 [4, 6]

nn.manual_seed(0)
let model = nn.Sequential([nn.Linear(2, 8), nn.Tanh(), nn.Linear(8, 1)])
let opt = nn.Adam(model.parameters(), {lr: 0.05})
let X = nn.tensor([[0, 0], [0, 1], [1, 0], [1, 1]])
let Y = nn.tensor([[0], [1], [1], [0]])
for step in 0..500 {
    opt.zero_grad()
    let loss = nn.binary_cross_entropy_with_logits(model.forward(X), Y)
    loss.backward()
    opt.step()
}
print(nn.no_grad(fn() => nn.sigmoid(model.forward(X))))   // XOR: about 0, 1, 1, 0
```

`nn` is PyTorch's core: Tensors that record how they were computed, `backward()` that gives each leaf its gradient, and the layers, losses and optimizers to train with. A Tensor holds an Array (`t.data`, float32 unless asked), so everything `array` does applies to it.

- Making them: `tensor(x, {requires_grad, dtype})`, `zeros`, `ones`, `rand`, `randn`, `full(shape, value)`, `arange`, each with those options; `manual_seed(n)` seeds the generator their random numbers come from (array's).
- Operators `+ - * / ** @`, `-t`, indexing (`t[0]`, `t[.., 1]`, `t[[0, 2]]`, `t[mask]`), and the comparisons (which give Arrays of Bools, and no gradient). Methods `sum`, `mean`, `max` (`dim?`, `{keepdim}`), `argmax`, `exp`, `log`, `sqrt`, `tanh`, `sigmoid`, `relu`, `abs`, `softmax(dim)`, `log_softmax(dim)`, `pow`, `matmul`, `clamp(lo, hi)`, `reshape`/`view`, `flatten`, `transpose(d0, d1)`, `permute(...)`, `unsqueeze`, `squeeze`, `T`; `item()`, `numpy()` (the data as an Array), `to_list()`, `detach()`, `astype(dtype)`.
- `t.backward(grad?, {retain_graph})` fills `.grad` (an Array) of every leaf with `requires_grad` it came from, adding to what is there; `zero_grad()` clears it. The graph is let go of as backward goes (as PyTorch's is: a second backward through it is an error unless the first kept it), so training loops don't grow. `no_grad(f)` runs `f` without recording anything (`set_grad_enabled(on)` too), and in-place updates, `add_(x, alpha?)`, `sub_`, `mul_`, `copy_`, `fill_`, `zero_`, record nothing either.
- Functions: `relu`, `leaky_relu(x, slope?)`, `gelu`, `silu`, `sigmoid`, `tanh`, `softmax`, `log_softmax`, `exp`, `log`, `sqrt`, `abs`, `clamp`, `sum`, `mean`, `max`, `matmul`, `cat(ts, dim?)`, `stack`, `linear(x, w, b?)`, `conv2d(x, w, b?, {stride, padding})`, `max_pool2d(x, k, {stride})`, `avg_pool2d`, `dropout(x, p?, training?)`, `layer_norm(x, w?, b?, {eps})`, `embedding(indices, w)`; losses `mse_loss`, `l1_loss`, `cross_entropy(logits, classes)`, `nll_loss`, `binary_cross_entropy`, `binary_cross_entropy_with_logits`, each with `{reduction: "mean" | "sum" | "none"}`.
- Modules, each with `forward(x)`, `parameters()`, `train()`, `eval()`, `state_dict()`, `load_state_dict(state)` and `zero_grad()`: `Linear(in, out, {bias})`, `Conv2d(in, out, k, {stride, padding, bias})`, `Embedding(n, dim)`, `LayerNorm(dim, {eps})`, `Dropout(p?)`, `ReLU`, `LeakyReLU`, `GELU`, `SiLU`, `Sigmoid`, `Tanh`, `Softmax(dim?)`, `LogSoftmax`, `Flatten`, `MaxPool2d(k, {stride})`, `AvgPool2d`, `Sequential([...])`, and the losses as modules (`MSELoss`, `L1Loss`, `CrossEntropyLoss`, `BCELoss`, `BCEWithLogitsLoss`: `forward(input, target)`). Starting weights are drawn as PyTorch draws them. A `Sequential` can hold objects of your own classes too: anything with `forward(x)` and `parameters()` methods.
- Optimizers `SGD(params, {lr, momentum, dampening, weight_decay, nesterov})`, `Adam(params, {lr, betas, eps, weight_decay})` and `AdamW` (decoupled weight decay), with `step()`, `zero_grad()`, `lr` and `set_lr(x)`. `params` is a list of Tensors or a module.
- `save(path, tensors)` writes an object of Tensors by name (or a module's `state_dict()`) as safetensors, the format of Hugging Face's models; `load(path)` reads one back as Tensors.
- Every operation's gradient is checked against PyTorch's (and against finite differences in the tests), and to the 6th decimal they are the same. Training is faster: an MNIST-sized network (784, 256, 10, batches of 64) trains twice as fast as PyTorch 2.15 on an M5, a small CNN 1.4 times, and a tiny network 14 times (Lube's calls cost less than Python's). float32's activations (sigmoid, tanh, GELU, SiLU) and softmax run without the C library, in loops that vectorize, on array's threads: within a few units in the last place, and GELU stays accurate far into its tail, where PyTorch's float32 GELU loses everything below -5.
- What PyTorch has and this hasn't: GPUs, recurrent layers (LSTM, GRU) and attention as modules (build them from the operations: `softmax(q @ k.T / 8) @ v`), BatchNorm, learning-rate schedulers, and higher-order gradients.

### Machine learning: `learn`

```lube
import array as np
import learn
let iris = learn.load_iris()
let split = learn.train_test_split(iris.data, iris.target, {test_size: 0.25, random_state: 0})
let X_train = split[0]
let X_test = split[1]
let y_train = split[2]
let y_test = split[3]
let forest = learn.RandomForestClassifier({n_estimators: 200, random_state: 0}).fit(X_train, y_train)
print(forest.score(X_test, y_test))     // 0.9736842105263158
let scale = learn.StandardScaler().fit(X_train)
let logit = learn.LogisticRegression().fit(scale.transform(X_train), y_train)
print(logit.predict(scale.transform(X_test[..3])))                   // [2, 1, 0]
print(np.round(logit.predict_proba(scale.transform(X_test[..1])), 3))  // [[0, 0.031, 0.969]]
print(learn.confusion_matrix(y_test, logit.predict(scale.transform(X_test))))
print(learn.cross_val_score(learn.KNeighborsClassifier(5), iris.data, iris.target))
                                        // [0.9666666666666667, 1, 0.9333333333333333, 0.9666666666666667, 1]
let km = learn.KMeans({n_clusters: 3, random_state: 0}).fit(iris.data)
print(np.bincount(km.labels_), np.round(km.inertia_, 4))             // [61, 50, 39] 78.8557
```

`learn` is scikit-learn's core on top of `array`: the same models, with the same names, methods, options and fitted attributes, taking and giving Arrays (nested lists of Numbers work too). Every number the example prints is the one sklearn prints for the same program (the forest's trees are its own, though its score is the same here).

- A model is made with its options in an object, sklearn's keyword arguments: `Ridge({alpha: 0.5})`, `KMeans({n_clusters: 3, random_state: 0})`; the one sklearn takes first may come bare: `Ridge(0.5)`, `KMeans(3)`, `PCA(2)`. An option it doesn't have, or a value it can't take, is an error that says which: *Ridge(): there is no option "alfa" (it has alpha, fit_intercept)*. `m.get_params()` gives them as an object and `m.set_params({...})` changes them; a model prints as sklearn's do, with the options given: `Ridge(alpha=0.5)`.
- `m.fit(X, y)` (`fit(X)` for what learns without labels) returns the model, so `m.fit(X, y).predict(X)` works. Then, as each model has them: `predict`, `predict_proba`, `predict_log_proba`, `decision_function`, `score` (accuracy for classifiers, R² for regressors, minus the inertia for KMeans), `transform`, `fit_transform`, `inverse_transform`, `fit_predict`. What fit found is a property: `coef_`, `intercept_`, `classes_`, `n_iter_`, `feature_importances_`, `cluster_centers_`, `labels_`, `inertia_`, `components_`, `explained_variance_ratio_`, `mean_`, `scale_`, `n_features_in_`... Using a model before `fit` is an error (*predict(): this Ridge is not fitted yet: call fit() first*), as is X with other features than fit had.
- Labels can be any Numbers: `classes_` are y's distinct values, sorted, and predictions come in y's dtype. `LabelEncoder` turns text labels (or any) into 0, 1, 2...
- Linear models: `LinearRegression`, `Ridge`, `Lasso`, `ElasticNet` (coordinate descent, stopping on the duality gap as sklearn does), each with one target or several (y with a column per target); `LogisticRegression` (L2, binary or multinomial, L-BFGS: `C`, `max_iter`, `tol`, `fit_intercept`) and `LinearSVC` (the squared hinge, one class against the rest).
- Neighbors: `KNeighborsClassifier` and `KNeighborsRegressor` (`n_neighbors`, `weights: "uniform"` or `"distance"`, `p: 1` or `2`), and `m.kneighbors(X, k?)`: `[distances, indexes]`. Up to 15 features they search a k-d tree; past that, a matrix product finds candidates and exact distances rank them. Equally near samples come in the order of the training data.
- Trees: `DecisionTreeClassifier` (`criterion: "gini"`, `"entropy"` or `"log_loss"`) and `DecisionTreeRegressor` (`"squared_error"`, `"friedman_mse"`), with `max_depth`, `min_samples_split`, `min_samples_leaf`, `max_features`, `random_state`; `get_depth()`, `get_n_leaves()`, `apply(X)` and `tree_` (sklearn's arrays: `feature`, `threshold`, `children_left`, `value`...). `RandomForestClassifier` and `RandomForestRegressor` add `n_estimators` and `bootstrap`, and build their trees on all the cores.
- And `GaussianNB` (`var_smoothing`), `KMeans` (k-means++ or `init: "random"`, `n_init`, `max_iter`, `tol`), `DBSCAN` (`eps`, `min_samples`), `PCA` (`n_components` a count or a share of the variance, `whiten`); `StandardScaler`, `MinMaxScaler` (`feature_range: [0, 1]`, `clip`), `OneHotEncoder` (`handle_unknown`, `drop`; its output is dense), `PolynomialFeatures` (`degree`, `interaction_only`, `include_bias`).
- Choosing models: `train_test_split(X, y..., {test_size, train_size, shuffle, random_state, stratify})` gives `[X_train, X_test, y_train, y_test...]` (lists stay lists); `KFold` and `StratifiedKFold` (`n_splits`, `shuffle`, `random_state`) have `split(X, y?)`, a list of `[train, test]` index Arrays; `cross_val_score(model, X, y, {cv, scoring})` fits a copy of the model on each fold (`cv` a count, stratified for classifiers as sklearn's is, or a KFold), scoring with `score()`, a metric's name (`"accuracy"`, `"f1_macro"`, `"neg_mean_squared_error"`...) or `fn(model, X, y)`. An object of your own class with `fit` and `score` works too; as an instance can't change itself when a plugin calls it, its `fit` returns the fitted model.
- Metrics: `accuracy_score`, `balanced_accuracy_score`, `precision_score`, `recall_score`, `f1_score`, `fbeta_score` (`{average: "binary" | "micro" | "macro" | "weighted" | "none", pos_label, labels, zero_division}`), `confusion_matrix`, `classification_report` (sklearn's table, as text), `log_loss`, `roc_auc_score` (two classes, or one against the rest), `mean_squared_error`, `root_mean_squared_error`, `mean_absolute_error`, `r2_score`, `explained_variance_score`, `silhouette_score`, `adjusted_rand_score`.
- Data: `load_iris({return_X_y})` (`{data, target, feature_names, target_names}`), `make_blobs`, `make_classification`, `make_regression`, `make_moons`, each `[X, y]`.
- Random numbers: each call that draws has a generator of its own, numpy's (array's kind). `random_state` seeds it, and gives the numbers `RandomState(random_state)` gives: the splits of `train_test_split`, `KFold` and `StratifiedKFold`, the data of `make_blobs`, `make_regression` and `make_moons`, and KMeans's starts are sklearn's. Without `random_state`, it is seeded from array's generator, so `np.seed(n)` makes a run repeat. Isolates fitting at once don't disturb each other.
- Speed, against scikit-learn 1.9 on an M5, with the same data: a random forest of 100 trees on 10,000 x 20 3 times as fast as sklearn's on all cores (18 times its default, one core), a decision tree 4 times, KMeans on 100,000 points 2.6 times, k nearest neighbors 2 times (50 features) to 5 times (4 features), multinomial logistic regression 1.7 times, binary the same, linear regression 10 times. Big products use the system's BLAS through array's `matmul`, and loops run on array's threads in fixed blocks, so results don't depend on how many there are.
- What differs from sklearn: when two splits of a tree (or two neighbors) are exactly as good, the first feature (or sample) wins, where sklearn draws at random, so a tree on data with such ties can differ; forests and the made-up `make_classification` draw their own numbers; LogisticRegression and LinearSVC reach sklearn's optimum to about `tol` (scale X for a fast, exact fit); `n_init` is `"auto"` as sklearn's is (one k-means++ start, ten random ones). Left out: sparse matrices, sample weights, `GridSearchCV`, `SGDClassifier`, `SVC` with kernels, gradient boosting, and the models beyond those above.

### Language models: `llm`

```lube
import llm
let m = llm.load("stories15M-q8_0.gguf")   // or llm.Model(path)
print(m.architecture, m.layers, m.vocab_size, m.context_length, m.quantization)   // llama 6 32000 128 Q8_0
print(m.generate("Once upon a time", {max_tokens: 24, temperature: 0}))
                                        // , there was a little girl named Lily. She loved to play outside in the sunshine. One day,
let ids = m.tokenize("Hello world")     // [1, 15043, 3186]: <s>, ▁Hello, ▁world
let logits = m.forward(ids)             // a float32 Array, one number for each token of the vocabulary
print(m.detokenize([logits.argmax()]))  // !

m.generate("The little dog", {temperature: 0.8, seed: 42, stop: ".", on_token: fn(piece) => write(piece)})
                                        //  was curious and loved to explore
let q = llm.load("qwen2.5-0.5b-instruct-q4_k_m.gguf")
print(q.generate("<|im_start|>user\nName three colors of the rainbow.<|im_end|>\n<|im_start|>assistant\n", {temperature: 0}))
                                        // The three colors of the rainbow are:
                                        //
                                        // 1. Red
                                        // 2. Orange
                                        // 3. Yellow
```

`llm` runs language models on the CPU, as llama.cpp does (and Hugging Face's transformers, for running a model): the GGUF files llama.cpp reads, which is what most downloads of open models are (`*.Q4_K_M.gguf`, `*-q8_0.gguf`...). It runs the llama family: Llama 1, 2 and 3, Mistral, TinyLlama, SmolLM, Qwen 2 and 3, Phi-3 and Gemma. The file is mapped into memory, its weights stay as they are (F32, F16, BF16, Q4_0, Q4_1, Q5_0, Q5_1, Q8_0, Q4_K, Q5_K or Q6_K), and the products run on array's threads.

- `load(path, options?)` gives a `Model`. The options: `threads` (0, the default: array's, so `array.threads(n)` sets it; or that many), `context` (how many tokens the model can hold at once: the model's own context length unless asked; the memory for them is taken as they come) and `mmap` (true: the file is mapped into memory, not read; false reads it, once).
- `m.generate(prompt, options?)` gives the text the model writes after the prompt: a str (with the model's BOS token, and special tokens in it, such as `<|im_start|>`, read as those tokens) or a List of token ids. It stops at the end of the text (the model's EOS, or a token such as `<|im_end|>` or `<|eot_id|>`), after `max_tokens` (256), when the context is full, or before a `stop` string (a str or a List of them). Then `temperature` (0.8; 0 takes the likeliest token every time), `top_k` (40; 0 for all), `top_p` (0.95) and `repeat_penalty` (1, none; 1.1 makes the last 64 tokens less likely), applied in that order, as transformers does. The draw is from array's generator: `seed` sets it first, and without it `array.seed(n)` makes a run repeat. `on_token` is a function given each piece of text as it comes (whole characters: a piece is never half of one), and false from it stops.
- A model keeps what it has read (the attention's cache). `m.forward(tokens)` reads tokens after those, and gives the logits for the token after the last; `m.tokens` lists what the cache holds, and `m.reset()` empties it. `generate` starts from its prompt alone, but what the cache holds of the prompt already is not read again, so a chat that grows turn by turn only reads what is new.
- `m.tokenize(text, {bos, eos, special})` gives token ids: with the model's BOS (and EOS) as the model has them, unless asked; `special: true` reads special tokens' text in it as those tokens. `m.detokenize(tokens, {special})` gives the text back (`special: true` shows tokens such as `</s>`). The tokenizer is the model's own, from the file: SentencePiece's (llama, mistral, phi3, gemma) or GPT-2's byte-level BPE with the model's rules for splitting text (llama 3, qwen2, smollm), and gives the token ids llama.cpp gives.
- What the file says of the model: `m.architecture`, `m.name`, `m.vocab_size`, `m.context_length` (what it was trained with), `m.context` (what it holds), `m.layers`, `m.embedding_length`, `m.heads`, `m.kv_heads`, `m.feed_forward_length`, `m.parameters`, `m.size` (the weights' bytes), `m.quantization` (`"Q4_K_M"`...), `m.bos` and `m.eos` (-1 if it has none), and `m.metadata`, all of it as an object. `m.tensors` lists the weights (`{name, type, shape}`) and `m.tensor(name)` gives one as a float32 Array.
- The products give the same numbers on every system, to the last bit: they multiply integers (the weights' and the vector's, quantized to 8 bits as llama.cpp does) and add up floats in a fixed order, in their NEON (ARM), SSE2 (x86-64) and plain C versions alike, so only the system's `exp`, `sin` and `cos` can make a last bit differ. Any number of threads gives the same logits, and so does a prompt read at once or a token at a time.
- An error says what is wrong with a file: *load(): 'model.gguf' has tensor 'blk.0.attn_q.weight' of type IQ2_XS, which llm cannot use*, *load(): 'old.gguf' is a GGUF file of version 1 (this reads versions 2 and 3)*.
- Speed, against llama-cpp-python 0.3 (llama.cpp's C++ underneath) on an M5, with the same files: TinyLlama 1.1B Q4_K_M generates 85 tokens a second to its 111 and reads a prompt at 307 to its 491; Qwen2.5 0.5B Q4_K_M generates 109 to its 163; stories15M about 2,000 to 3,100. A model in numpy (its weights in float32, products by Accelerate) generates 15 tokens a second with TinyLlama, and 700 with stories15M.
- Left out: GPUs, the architectures beyond those above (Gemma 2 and 3, Phi-2, mixtures of experts such as Mixtral), the formats Q2_K, Q3_K and the IQ ones, Hugging Face's own checkpoints (safetensors: convert them with llama.cpp's `convert_hf_to_gguf.py`), chat templates (write the prompt as the model expects it), and YaRN's long contexts.

### Web pages: `web`

Pages made of components, served by Lube itself over HTTP, safe by default. A page's view is HTML from the server. Its handlers stay on the server: the page's small script sends each event over a WebSocket, the handler runs, and the page gets the new view.

```lube
import web.*

fn Todos() component {
  var items = str[]
  fn add(fields) {                                 // submit: the form's fields
    if trim(fields.item) != "" { items.push(trim(fields.item)) }
  }
  fn clear() { items = str[] }
  return main(
    h1("To do ({len(items)})"),
    form({onsubmit: add}, field({name: "item"}), button("Add")),
    ul(items.map(fn(item) => li(item))),           // text is escaped: "<b>" shows as <b>
    button({onclick: clear, disabled: len(items) == 0}, "Clear")
  )
}

let app = App()                                    // 127.0.0.1 unless told otherwise
app.page("/", fn(req) => Todos(), {title: "To do", styles: ["/assets/app.css"]})
app.get("/hello/:name", fn(req) => "hello {req.params.name}")
app.post("/api/echo", fn(req) => {status: 201, type: "application/json", body: req.text})
app.static("/assets", "public")
app.serve(8080)                                    // until a handler calls app.stop()
```

- **Views.** `div(attributes?, children...)`, and the same for `p`, `span`, `a`, `button`, `form`, `ul`, `li`, `table`, `h1` to `h6` and the rest of HTML's elements. `<input>` is `field()`, because `input` is Lube's own. `el("svg", ...)` makes any other element. A child is text, a Number, a Node, a component (its view), a List of those, or nothing. Attributes are text, Numbers or Bools (`true` gives the attribute alone, `false` leaves it out). `html(view)` gives the HTML as text.
- **Lists.** `key:` on a list's items (`li({key: item.id}, ...)`) lets the page move them as the list changes, instead of rewriting them.
- **In the browser.** The page's script, under 5 KB, makes the page match each new view in place, changing only what differs. Focus, the caret and what's being typed stay as they are. A field takes the server's value when that value changes, but not while someone types in it, unless its form was just sent: so a form whose view leaves its fields empty clears after it is sent. Events go one at a time, at most one every 60 ms. A field's latest value replaces one still waiting, so typing fast sends what was typed, not every key. While an event waits to go or for its answer, the page's root has `data-busy`. When the server goes away, it gets `data-closed`. Both are there for styles to show.
- **Handlers.** `onclick`, `ondblclick`, `oninput`, `onchange`, `onsubmit`, `onkeydown`, `onkeyup`, `onfocus`, `onblur`, `onmouseenter` and `onmouseleave` take a function: a component's handler, or a closure. `input` and `change` give it the element's value (a Bool for a checkbox), `submit` the form's fields as an object, and the key events the key's name. The other events give nothing.
- **Pages.** `app.page(path, fn(req), {title, styles, scripts}?)` serves what `fn` gives, a component or a view, as a page. A page with a component or handlers is live: it keeps its component until its browser tab goes, and then runs its `on leave` blocks. `styles` and `scripts` are paths on the site.
- **Other routes.** `app.get`, `post`, `put`, `patch` and `delete` take `fn(req)`. It returns text (`text/plain`), `Bytes`, a view (HTML), nothing (204), or `{status, body, type, headers}`. Paths take `:name` for one part and `*rest` at the end for the rest. The request is `{method, path, params, query, headers, body, text, form, ip}`: `body` is its `Bytes`, `text` the same as text, and `form` a urlencoded form's fields.
- **Static files.** `app.static(prefix, folder)` serves the folder's files under `prefix`, with their content types.
- **Serving.** `app.serve(port?)` serves until `app.stop()`, or until Ctrl-C (or SIGTERM), which stops it the same way: its pages leave, running their `on leave` blocks, and `serve()` returns, so the program can finish its own way. Another Ctrl-C before that is done ends the program at once. (When the program, a debugger or a host program has its own handler for the signal, or ignores it, `serve()` leaves it so.) `app.start(port)` listens (port 0 picks a free one, and `start` returns it) and `app.step(seconds)` serves what has come, so a program can serve from its own loop. All of it runs on the program's thread, inside `step()`. `app.pages()` counts the live pages. Errors go to standard error, as `web: GET /path: what happened`.

`examples/web/todo.lube` is a whole app: a to-do list with items that are components of their own, a filter that updates as you type, and a stylesheet. Run `lube examples/web/todo.lube` in Lube's folder, then open http://localhost:8080/. (`lube examples/web/todo.lube 3000` serves on port 3000.)

The policy is on from the start. `App({policy: {...}})` loosens a rule by name, and an unknown name is an error, so a typo can't weaken one:

- **Strict requests.** Lines end with CRLF. There's no space before a header's colon and no folded header, one `Host` and one `Content-Length` of digits, no `Transfer-Encoding`, and no `..` or `.` in a path. %-escapes must be well formed and the text UTF-8. These are the gaps that request smuggling and path tricks use.
- **Limits.** 16 KB of head, 100 headers, an 8 KB URL and a 1 MB body. A client has 10 seconds for a head, 30 for a body, and 5 idle between requests. There are 512 connections in all and 64 from one address. A client that stops reading its answer is dropped. Policy: `max_header`, `max_headers`, `max_url`, `max_body`, `header_timeout`, `body_timeout`, `keep_alive`, `max_connections`, `max_per_ip`.
- **Security headers.** Every answer carries a Content-Security-Policy (scripts, styles and images from the site only, no inline code, no framing by other sites), `X-Content-Type-Options: nosniff`, `Referrer-Policy: same-origin`, Cross-Origin-Opener-Policy, Cross-Origin-Resource-Policy and `X-Frame-Options: DENY`. A handler can't set these, nor a header that holds a line break. Policy: `csp`, and `inline_styles: true` for `style="..."`, which is refused otherwise.
- **Escaping.** Text and attribute values are escaped, and only `raw(html)` goes out as it is. Nodes come from the helpers alone, so data such as a request's JSON can't become markup. `<script>`, `<style>` and `<base>` are refused. `on...` attributes take functions, never code. URLs in `href`, `src`, `action` and the like keep only `http:`, `https:`, `mailto:`, `tel:` and paths on the site: `javascript:`, `data:` and the rest become `about:invalid#refused`.
- **Static files.** A static file comes from its folder only: no `..`, no names that start with `.` (such as `.env` or `.git`), no symbolic links, and no folder listings.
- **WebSockets.** A page's WebSocket must come from the page's own site (`Origin`), name the page's secret, and carry the session's cookie, which is `HttpOnly` and `SameSite=Strict`. Handler ids are secret per page. Events are limited to 20 a second (bursts of 50) and 64 KB each, and a socket that breaks the protocol is closed. Policy: `origins` (other sites allowed, such as `["https://example.com"]`), `events_per_second`, `event_burst`, `max_message`, `ws_idle`, `max_pages`, `page_wait`, `secure_cookies`.
- **Host names.** The server answers to addresses (`127.0.0.1`, `[::1]`, `10.0.0.5`), to `localhost`, and to the names in policy `hosts` (such as `["example.com"]`, or `["*"]` for any). Any other `Host` gets 421. A page of another site whose name was made to lead here (DNS rebinding) gets nothing.
- **Forgery.** A POST, PUT, PATCH or DELETE from a page of another site gets 403. That covers an `Origin` that isn't this site's or one of policy `origins`, and `Sec-Fetch-Site: cross-site`. Clients that aren't browsers send neither header, and pass.
- **Errors.** A handler's error answers a bare `500 Internal Server Error`. The error itself goes to the log, and to the browser only with `App({dev: true})`.
- **TLS and proxies.** The server listens on `127.0.0.1` unless `App({host: "0.0.0.0"})` says otherwise. It doesn't speak TLS: put it behind a proxy that does, and say `trust_proxy: true`. Then `req.ip` comes from `X-Forwarded-For`, and `X-Forwarded-Proto: https` makes the cookie `Secure`. Without `trust_proxy`, those headers mean nothing.

### Writing a plugin

A plugin is a shared library with one entry point. It includes `lube_plugin.h` (in Lube's `src/`), which is all it needs: everything goes through the API table that Lube passes it, so it links against no part of Lube.

```c
// maths.c
#include "lube_plugin.h"

static const LubeAPI *lube;

static LubeValue twice(LubeVM *vm, int argc, const LubeValue *argv) {
    (void)argc;  /* (always 1: declared below) */
    if (lube->kind(argv[0]) != LUBE_NUMBER)
        return lube->error(vm, "twice() needs a Number, not %s", lube->a_type(argv[0]));
    return lube->number_value(2 * lube->number(argv[0]));
}

LUBE_PLUGIN(maths) {                      // the plugin "maths": maths.so, .dylib or .dll
    lube = api;
    lube->function(plugin, "twice", twice, 1, 1);
    return LUBE_PLUGIN_API;
}
```

```sh
cc -shared -fPIC -I lube/src -o plugins/maths.so maths.c   # Linux; macOS: -dynamiclib -o plugins/maths.dylib;
                                                            # Windows: -shared -o plugins/maths.dll
```

```lube
import maths
print(maths.twice(21))   // 42
```

- Lube looks for `maths.so` (`.dylib`, `.dll`) in the folders of `LUBE_PLUGINS`, then in `plugins/` next to `lube` (or next to a built program), then in `../lib/lube/plugins/` from `lube` (where `make install` puts Lube's own). A package folder of the same name comes first.
- A function gets its arguments borrowed and returns a new value, or `lube->error(vm, ...)` to raise an error. The API reads values (`kind`, `number`, `str`, `bytes`, `list_get`, `object_get`...) and makes them (`new_str`, `new_bytes`, `new_list`, `new_object`...).
- `lube->type(plugin, "Counter", size, release, describe)` declares a type whose objects carry `size` bytes of the plugin's own data: `release` lets go of what that data holds when the last owner lets go, and `describe` says how it prints. `lube->method(...)` adds its methods and `lube->constructor(...)` makes `Counter(...)` work.
- The entry point runs once per process; its functions can run in any isolate, so shared state goes under a lock (`lube->lock_new()`). `LUBE_PLUGIN_API` is the version of the API the plugin was made for: a newer Lube loads it, and an older one says it needs a newer Lube.
- Version 2 of the API added what Arrays and Tensors needed. `lube->op(type, "+", fn)` gives a type an operator: `+ - * / % ** @`, the comparisons, `"neg"`, `"[]"` and `"[]="` (indexing, with several indexes as several arguments), `"len"` (and with `"[]"`, `for x in t`), `"copy"` (which makes `x[i] = v` copy a shared object first, as lists do), and `+= -= *= /= %=` in place when nothing else holds the object. `lube->property(type, "shape", fn)` makes `x.shape` work, `lube->constant(plugin, "pi", 3.14...)` gives the package a Number, and `lube->range(v, ...)` reads a range. `lube->call(vm, f, argc, argv)` and `lube->call_method(vm, obj, "forward", ...)` call back into Lube (`lube->failed(r)` says whether that raised an error), and `lube->object_keys(vm, obj)` lists an object's keys.
- One plugin can use another's: `lube->provide(plugin, &table)` gives other plugins a table of the plugin's own, and `lube->require(plugin, "array")` loads that plugin and returns its table (`src/plugins/array.h` is array's: `nn` makes and reads Arrays through it). A program built with `lube --build` takes along the plugins its plugins require.
- Version 3 added `lube->take_error(vm, buf, size)`: it catches the error a `call` failed with, as text (`file:line: message`), so a plugin can carry on (the web plugin answers 500 and goes on serving). `call_method` reaches a component's handlers, `view` and `leave` too.
- `src/lube_plugin.h` describes every function, and Lube's own plugins (`src/plugins/fs.c`, `net.c`, `audio.c`, `gfx.c` and the rest) are complete examples; `tests/plugins/vec.c` uses every part of version 2.

### Errors

```lube
try {
  risky()
} catch err {
  print("failed:", err)
}

throw "something broke"                  # throw any value
throw {code: 404, message: "not found"}
```

Runtime errors such as `1 / 0`, a bad index or an undefined variable can be caught. The caught value is the message, as a `str`. An uncaught error prints its location and a traceback:

```
average.lube:2: error: division by zero
    2 |   return a / b
traceback (innermost call first):
  at divide (average.lube:2)
  at average (average.lube:5)
  at <script> (average.lube:8)
```

Runaway recursion is a catchable error too, and its traceback folds the repeats:

```
countdown.lube:3: error: stack overflow: too much recursion
    3 |   return countdown(n - 1)
traceback (innermost call first):
  at countdown (countdown.lube:3)
  ... repeated 106251 more times
  at launch (countdown.lube:5)
  at <script> (countdown.lube:6)
```

### Modules

```lube
import "vector"      # runs vector.lube (next to this file) once
```

Imported files share the global scope, so their top-level functions and variables become available. A file runs once, however its path is spelled (`lib/x` and `./lib/../lib/x` are the same file).

### Packages

A bigger program can keep its parts apart in packages. A package is a folder: every file in `lube/geometry/` starts with the line `package lube.geometry`, and together those files are the package. Its names belong to it, so two packages can both have a `Point` or a `len`:

```lube
// lube/geometry/point.lube
package lube.geometry
class Point { ... }
fn _round(x: Number) => ...          // names starting with _ stay inside the package

// lube/geometry/area.lube
package lube.geometry
fn area(p: Point): Number => ...     // the package's other files need no import

// main.lube
import lube.geometry.Point           // one name
import lube.geometry.area as size    // ...under another name
import lube.text.*                   // every public name of a package
import lube.geometry                 // the package itself: geometry.Point, geometry.area(p)
import lube.geometry as geo          // ...under another name

let p = Point(3, 4)
print(size(p), geometry.area(p), p is geo.Point)
```

- Packages are found from the main program's folder, then from the folders listed in `LUBE_PATH` (separated by `:`, or `;` on Windows). Importing anything from a package loads its whole folder, once.
- Inside a package, a name means: an imported name, else one of the package's own, else a built-in. So a package can define its own `len` without affecting anyone else.
- A file's own top-level names come before those of `import x.*`: with `import web.*`, a file's own `header` or `var b` is its own, and `web.header` is still there by name. Importing one name the file declares too (`import web.div` next to `fn div()`) is an error.
- `import "file"` still works as before: a file without a package line shares the importer's names. A file that has one keeps its names to itself, however it is imported.
- Package imports go at the top level of a file. A missing package or name, a private name, or a file in a package's folder whose package line disagrees, is an error before the program runs.
- Isolates, hot reload and `lube --build` all handle packages: a built executable carries the package files it uses.

### Hot reload

`lube --hot app.lube` runs a program and applies your edits while it keeps running. Save a file, and within about 50 ms of run time its new code is in the running program:

```
$ lube --hot game.lube
...
lube: reloaded game.lube
```

- **Functions** are replaced: the next call runs the new code.
- **Classes** get their new methods, constructor and fields, and existing instances use the new methods right away. (An existing instance keeps the fields it was made with; new instances get the new ones.)
- **Variables keep their values.** A `var` or `let` that already exists isn't run again, so the program's state survives. New globals are created, and new imports run.
- **Nothing else runs again:** top-level statements like a main loop keep running with the new functions.
- A call that is running when its file changes finishes with the old code.
- An edit with an error is reported, and the program goes on with the code it had. When you fix the file, the fix is applied.

A program can also apply changes itself, at a moment it chooses: `hot_reload()` looks at every file the program was loaded from, applies the ones that changed, and returns their paths. `--hot` looks while the program runs code, so a program waiting in `input()` gets the changes when it continues.

### Debugging

`lube --debug` runs a program under the debugger. It stops before the first line and takes commands at a `(lube)` prompt (`h` lists them):

```sh
./lube --debug app.lube [args...]
```

For example, with this program in `shop.lube`:

```lube
fn total(items) {
  var sum = 0
  for item in items {
    sum += item.price * item.qty
  }
  return sum
}

let cart = [{name: "pen", price: 2, qty: 3}, {name: "book", price: 12, qty: 1}]
print("total:", total(cart))
```

```text
$ lube --debug shop.lube
(lube) b 4 if item.name == "book"      # stop at line 4, only for the book
(lube) c                               # run until then
   at total (shop.lube:4)
    4 |     sum += item.price * item.qty
(lube) locals                          # see the variables
items = [{name: "pen", price: 2, qty: 3}, {name: "book", price: 12, qty: 1}]
sum = 6
item = {name: "book", price: 12, qty: 1}
(lube) p item.price * item.qty         # evaluate any expression here
12
(lube) bt                              # how we got here
=>#0 total (shop.lube:4)
  #1 <script> (shop.lube:10)
(lube) n                               # next line
(lube) p sum
18
(lube) c                               # run to the end
total: 18
```

| What you want | Command |
|---|---|
| Run to the next breakpoint | `c` |
| Next line / step into a call / step out of the function | `n` / `s` / `o` |
| A breakpoint | `b 12`, or in another file: `b lib/util.lube:30` |
| ...only when a condition holds | `b 12 if total > 100` |
| ...only on certain hits | `b 12 hits 5` (the 5th), `hits >= 5`, `hits % 10` (every 10th) |
| A breakpoint that removes itself after one stop | `tb 12` |
| Print a message there without stopping | `log 12 total is {total}` |
| List or delete breakpoints | `bl`, `d 2`, `d` (all of them) |
| Evaluate an expression | `p expr` |
| See variables | `locals`, `globals` |
| The calls that led here, and moving through them | `bt`, then `up`, `down`, `frame 2` |
| The source around the current line | `l` |
| Stop where errors are raised | `catch all` (the default stops only for uncaught errors), `catch off` |
| Quit | `q` |

- **Ctrl-C** pauses a running program, which is handy for finding where an endless loop is stuck.
- An uncaught error stops the program where it's raised, with every frame still there to inspect.
- After `up`, `p` and `locals` work in the frame you moved to.
- Paused inside an `on` block, you see the block's variables and those of the function it runs on.
- **Ctrl-D**, or the end of piped commands, leaves the debugger and lets the program run on to the end.

Without the debugger, an uncaught error prints a traceback with the file and line of every call; `lube --check app.lube` reports syntax errors without running anything; and `print(repr(x))` shows a value exactly, quotes included.

## Standard library

All of these are globals, and all of them can be called in method style; the rows for plugins (`fs`, `net`, `audio`, `math`...) are those plugins' names, after their import (see Plugins). The methods of `Bytes` and of the plugins' types, such as `b.pack(...)` and `f.read()`, are methods only.

| area | functions |
|---|---|
| change a list or object in place (take `&`) | `push(&xs, ...)`, `pop(&xs)`, `insert(&xs, i, x)`, `remove(&xs, i)`, `delete(&obj, key)` (returns whether the key was there), `clear(&x)`; method style needs no `&`: `xs.push(4)` |
| output and input | `print(...)`, `write(...)` (no newline), `input(prompt?)` (an error at end of input), `read_file(path)`, `write_file(path, text)`, `append_file(path, text)`, `exit(code?)` |
| types | `str(x)`, `String(x)`, `Number(x, fallback?)`, `Bool(x)`, `Object(x)`, `Map(x)`, `List(x)`, `Function(f)`, `Range(...)`, plus the sized integers; `type(x)` gives the type's name; the lowercase `num`, `int`, `bool` and `list` still work; `repr(x)` |
| sized integers | `int8`, `int16`, `int32`, `int64`, `uint8`, `uint16`, `uint32`, `uint64`; each takes an optional value and defaults to 0 |
| lists | `len`, `slice(x, start, end?)`, `reverse`, `sort(xs, compare_or_key?)`, `contains`, `index_of`, `join(xs, sep?)`, `list(x)`, `zip(a, b)` |
| objects | `keys`, `values`, `items`, `has(obj, key)`, `get(obj, key, default?)` (a missing key without a default is an error) |
| functional | `map`, `filter`, `each`, `reduce(xs, f, start?)`, `find(xs, f, default?)`, `find_index` (-1 if none), `any(xs, f?)`, `all(xs, f?)`, `sum`, `min`, `max`, `range(end)` / `range(start, end, step?)` |
| strings | `upper`, `lower`, `trim`, `split(s, sep?)`, `replace(s, old, new)`, `starts_with`, `ends_with`, `chars`, `ord`, `chr`, `pad_left(x, width, fill?)`, `pad_right`, `fixed(n, digits?)` |
| math | `abs`, `floor`, `ceil`, `round(x, digits?)`, `sqrt`, `pow`, `sin`, `cos`, `tan`, `atan2`, `log(x, base?)`, `exp`, `PI`, `INF` |
| random | `random()`, `random_int(lo, hi)`, `choice(xs)`, `shuffle(xs)`, `seed(n)` |
| reflection | `class_of`, `parent_of`, `fields`, `methods`, `signature(f)` / `signature(class, method)`, `get_field`, `set_field(&x, name, value)`, `call_method(x, name, ...)`, `has_method`, `classes()`, `find_class(name)`, `globals()` (see Reflection) |
| concurrency | `spawn(f, args...)`, `iso.join()`, `channel(n?)`, `send`, `receive`, `try_receive(ch, default)`, `take(ch, n)`, `close`, `cores()`, `parallel_map(xs, f)` (see Concurrency) |
| bytes | `Bytes(x?)`, `len`, `push(&b, ...)`, `+`, `in`; methods `b.pack(format, n, at?)`, `b.unpack(format, at?)`, `b.decode()`, `b.hex()`, `b.slice(from, to?)` (see Bytes and time) |
| money | `m(amount?, currency?)` / `Money(...)`, `currency(code?)`, `add_currency(code, symbol, decimals)`; methods `x.split(n)`, `x.allocate(ratios)`, `x.to(currency, rate)`, `x.round(decimals?)`, `x.format(options?)`; `x.amount`, `x.units`, `x.currency`, `x.symbol`, `x.decimals` (see Money) |
| `math` (plugin) | `sqrt`, `cbrt`, `exp`, `exp2`, `expm1`, `log(x, base?)`, `log2`, `log10`, `log1p`, `pow`, `sin`, `cos`, `tan`, `asin`, `acos`, `atan`, `atan2`, `sinh`, `cosh`, `tanh`, `asinh`, `acosh`, `atanh`, `degrees`, `radians`, `hypot(...)`, `dist(p, q)`, `floor`, `ceil`, `trunc`, `modf`, `frexp`, `ldexp`, `fmod`, `remainder`, `copysign`, `fabs`, `fma`, `fmax`, `fmin`, `nextafter(x, y, options?)`, `ulp`, `isfinite`, `isinf`, `isnan`, `isnormal`, `issubnormal`, `signbit`, `isclose(a, b, options?)`, `fsum(xs)`, `prod(xs, options?)`, `sumprod(xs, ys)`, `factorial`, `comb`, `perm(n, k?)`, `gcd(...)`, `lcm(...)`, `isqrt`, `gamma`, `lgamma`, `erf`, `erfc`; `pi`, `e`, `tau`, `inf`, `nan` (see Maths) |
| `cmath` (plugin) | `Complex(re, im?)`, `Complex(text)`, `sqrt`, `exp`, `log(z, base?)`, `log10`, `sin`, `cos`, `tan`, `asin`, `acos`, `atan`, `sinh`, `cosh`, `tanh`, `asinh`, `acosh`, `atanh`, `phase`, `polar`, `rect(r, phi)`, `abs`, `isfinite`, `isinf`, `isnan`, `isclose(a, b, options?)`, `infj()`, `nanj()`; `pi`, `e`, `tau`, `inf`, `nan`; `z.real`, `z.imag`, `z.conjugate()`, and `+ - * / **`, `==`, `!=`, `-z` (see Maths) |
| `random` (plugin) | `seed(a?, version?)`, `random()`, `uniform(a, b)`, `triangular(low?, high?, mode?)`, `randint(a, b)`, `randrange(start, stop?, step?)`, `getrandbits(k)`, `randbytes(n)`, `choice(xs)`, `choices(xs, weights?, {weights, cum_weights, k})`, `sample(xs, k, {counts})`, `shuffle(xs)` (a new list), `gauss`, `normalvariate`, `lognormvariate`, `expovariate`, `vonmisesvariate`, `gammavariate`, `betavariate`, `paretovariate`, `weibullvariate`, `binomialvariate`, `getstate()`, `setstate(s)`, `Random(seed?)` and `SystemRandom()` (with all of these as methods); `BPF`, `RECIP_BPF`, `LOG4`, `NV_MAGICCONST`, `SG_MAGICCONST`, `TWOPI` (see Statistics and random numbers) |
| `statistics` (plugin) | `mean`, `fmean(data, weights?)`, `geometric_mean`, `harmonic_mean(data, weights?)`, `median`, `median_low`, `median_high`, `median_grouped(data, interval?)`, `mode`, `multimode`, `quantiles(data, {n, method})`, `variance(data, xbar?)`, `pvariance(data, mu?)`, `stdev(data, xbar?)`, `pstdev(data, mu?)`, `covariance(x, y)`, `correlation(x, y, {method})`, `linear_regression(x, y, {proportional})` (`{slope, intercept}`), `kde(data, h, kernel?)`, `kde_random(data, h, kernel?, {seed})`, `NormalDist(mu?, sigma?)`, `from_samples(data)`; methods `d.pdf(x)`, `d.cdf(x)`, `d.inv_cdf(p)`, `d.overlap(e)`, `d.quantiles(n?)`, `d.samples(n, {seed})`, `d.zscore(x)`, `k.pdf(x)`, `k.cdf(x)`, `r.sample()`, `r.samples(n)` (see Statistics and random numbers) |
| `array` (plugin) | `array`, `zeros`, `ones`, `full`, `arange`, `linspace`, `eye`, `concatenate`, `stack`...; elementwise `sqrt`, `exp`, `log`, `sin`...; `sum`, `mean`, `std`, `min`, `max`, `argmax`, `median`...; `sort`, `argsort`, `unique`, `where`; `matmul`, `solve`, `inv`, `det`, `svd`, `qr`, `eigh`, `lstsq`, `norm`...; `seed`, `rand`, `randn`, `randint`, `normal`, `choice`, `permutation`; `save`, `load`, `loadtxt`, `savetxt`, `load_safetensors`, `save_safetensors`; `blas`, `use_blas`, `threads`; `a.shape`, `a.dtype`, `a.T`, `a.reshape(...)`, `a.astype(t)`, `a[i, j]`, `a @ b` (see Arrays) |
| `nn` (plugin) | `tensor`, `zeros`, `ones`, `rand`, `randn`, `manual_seed`, `no_grad(f)`; `relu`, `gelu`, `softmax`, `cross_entropy`, `mse_loss`, `conv2d`, `layer_norm`, `embedding`...; `Linear`, `Conv2d`, `Embedding`, `LayerNorm`, `Dropout`, `ReLU`..., `Sequential`; `SGD`, `Adam`, `AdamW`; `save`, `load`; `t.backward()`, `t.grad`, `t.item()`, `m.forward(x)`, `m.parameters()`, `opt.step()` (see Neural networks) |
| `learn` (plugin) | models `LinearRegression`, `Ridge`, `Lasso`, `ElasticNet`, `LogisticRegression`, `LinearSVC`, `KNeighborsClassifier`, `KNeighborsRegressor`, `DecisionTreeClassifier`, `DecisionTreeRegressor`, `RandomForestClassifier`, `RandomForestRegressor`, `GaussianNB`, `KMeans`, `DBSCAN`, `PCA`, `StandardScaler`, `MinMaxScaler`, `LabelEncoder`, `OneHotEncoder`, `PolynomialFeatures` (options as an object); methods `m.fit(X, y?)`, `m.predict(X)`, `m.predict_proba(X)`, `m.score(X, y)`, `m.transform(X)`, `m.fit_transform(X)`, `m.inverse_transform(X)`, `m.get_params()`, `m.set_params(opts)`; `train_test_split`, `KFold`, `StratifiedKFold`, `cross_val_score`; `accuracy_score`, `precision_score`, `recall_score`, `f1_score`, `confusion_matrix`, `classification_report`, `mean_squared_error`, `r2_score`, `log_loss`, `roc_auc_score`...; `load_iris`, `make_blobs`, `make_classification`, `make_regression`, `make_moons` (see Machine learning) |
| `llm` (plugin) | `load(path, options?)` or `Model(path, options?)`; methods `m.generate(prompt, options?)`, `m.forward(tokens)`, `m.tokenize(text, options?)`, `m.detokenize(tokens, options?)`, `m.reset()`, `m.tensor(name)`; properties `m.architecture`, `m.name`, `m.vocab_size`, `m.context_length`, `m.context`, `m.layers`, `m.embedding_length`, `m.heads`, `m.kv_heads`, `m.feed_forward_length`, `m.parameters`, `m.size`, `m.quantization`, `m.bos`, `m.eos`, `m.tokens`, `m.metadata`, `m.tensors` (see Language models) |
| `audio` (plugin) | `Sound(path or bytes)`, `Sound(samples, rate?, channels?)`, `stop_all()`, `set_volume(x)`, `device()`; methods `s.play(volume?, pan?)`, `s.loop(volume?, pan?)`, `s.duration()`, `s.rate()`, `s.channels()`, `s.samples()`, `s.to_wav()`; `v.stop()`, `v.is_playing()`, `v.set_volume(x)`, `v.set_pan(x)`, `v.set_speed(x)` (see Sound) |
| `net` (plugin) | `tcp_listen(port, host?)`, `tcp_connect(host, port, seconds?)`, `udp_open(port?, host?)`, `poll(sockets, seconds?)`; methods `s.accept(seconds?)`, `s.receive(seconds?)`, `s.send(data)`, `s.packets(seconds?)`, `s.send_to(host, port, data)`, `s.is_open()`, `s.port()`, `s.peer()`, `s.close()` (see Sockets) |
| `fs` (plugin) | `open(path, mode?)`; methods `f.read(n?)`, `f.read_line()`, `f.write(x)`, `f.seek(pos, from?)`, `f.tell()`, `f.size()`, `f.eof()`, `f.flush()`, `f.close()`; `read_bytes`, `write_bytes`, `exists`, `is_dir`, `list_dir`, `make_dir`, `remove_file`, `rename_file`, `file_size` |
| misc | `assert(cond, message?)`, `clock()` (CPU seconds), `time()` (Unix time), `now()` (a steady clock, for timing), `sleep(seconds)`, `hot_reload()` (see Hot reload), `args` |

Everything outside the first row returns a new value and leaves its arguments alone; `sort` and `reverse` return new lists. `map`, `filter` and `each` also pass the index when the function takes two parameters: `xs.map(fn(x, i) => ...)`. A `sort` comparator can return a number (negative means `a` goes first) or a bool (`true` means `a` goes first). A function of one item is a key instead, as Python's `key=` is: `people.sort(fn(p) => p.age)` calls it once for each item, then orders the items by their keys (equal ones keep their order). That is much faster than a comparator, which is called for each pair the sort compares.

Lengths and indexes count bytes. Iterating text, `chars()`, `ord()`, `chr()` and a `String`'s `length` understand UTF-8.

## Embedding: Lube inside C and C++

A C or C++ program can run Lube: a game's scripts, a tool's configuration, a program's own plugins. `make lib` builds `liblube.a` (all of Lube but its command line), and `src/lube_embed.h` is the API:

```c
// game.c
#include "lube_embed.h"
#include <stdio.h>

static LubeValue add(LubeVM *vm, int argc, const LubeValue *argv) {  // a function of the program's, for Lube
    const LubeAPI *lube = lube_api();
    (void)vm;
    (void)argc;
    return lube->number_value(lube->number(argv[0]) + lube->number(argv[1]));
}

int main(void) {
    const LubeAPI *lube = lube_api();
    LubeVM *vm = lube_open();
    lube_function(vm, "add", add, 2, 2, NULL);
    if (lube_run(vm, "fn update(dt) => add(dt, 1)\nprint(\"loaded\")", "game.lube") != LUBE_OK)
        fprintf(stderr, "%s", lube_error_message(vm));
    LubeValue dt = lube->number_value(0.5), result;
    if (lube_call_global(vm, "update", 1, &dt, &result) == LUBE_OK) {
        printf("%g\n", lube->number(result));  // 1.5
        lube->release(result);                  // what a call gives you is yours
    }
    lube_close(vm);
    return 0;
}
```

```sh
cc game.c -I lube/src lube/liblube.a -lm -lpthread   # Linux: add -ldl. Windows: -lm (and with the plugins
                                                     # inside: -lws2_32 -lgdi32 -luser32)
```

- Code runs as files do, one after another in the same interpreter, so what one run defines the next can use. `lube_run(vm, code, name)` runs text and `lube_run_script(vm, path)` a file. Both return `LUBE_OK`, `LUBE_ERROR` or `LUBE_EXIT`.
- An error never ends the program. `lube_error_message(vm)` gives the message as Lube prints it, with the line and traceback. A script's `exit(n)` ends the script, not the program: no `catch` stops it, the call returns `LUBE_EXIT`, and `lube_exit_code(vm)` gives `n`. Only running out of memory ends the process, after calling the function given to `lube_on_fatal`.
- `lube_get` and `lube_set` read and set globals. `lube_call_global` and `lube_call_value` call Lube's functions (built-ins too). Values are `LubeValue`, read and made with `lube_api()`, the same functions plugins use (see Writing a plugin).
- `lube_function(vm, name, fn, min, max, data)` gives Lube a function of the program's, and `lube_api()->function_data(vm)` gives `data` back inside it. For types with methods (a game's entities), write a plugin in the program itself and add it with `lube_plugin_add("game", lube_plugin_init_game)`: scripts then `import game`.
- `lube_set_output` sends what Lube prints to the program. Without it, what scripts print goes to stdout, and errors only to `lube_error_message`.
- Each call into Lube measures the stack the calling thread has left, so a script that recurses too deeply gets an error, even on a thread with a small stack. One thread uses an interpreter at a time; several interpreters can run side by side, and a script's isolates run on threads of their own.
- Plugins come along as for `lube`: as files (`plugins/` next to the program, or `LUBE_PLUGINS`), or inside the library with `make clean lib STATIC_PLUGINS=1`.

From C++, `lube_embed.h` works as it is, since it is `extern "C"`. `src/lube_embed.hpp` adds classes over it: `lube::VM` closes itself, `lube::Value` lets go of its value, functions can be lambdas, and errors are exceptions:

```cpp
#include "lube_embed.hpp"

int score = 0;
lube::VM vm;  // (before the values: they go first)
vm.def("add_score", [&score](lube::VM &, const std::vector<lube::Value> &args) {
    score += static_cast<int>(args[0].number());
    return lube::Value(score);
}, 1, 1);
try {
    vm.run_script("level1.lube");
    vm.call("on_hit", {vm.str("goblin"), lube::Value(3)});
} catch (const lube::Error &e) {
    std::cerr << e.what();  // as Lube says it: the line and the traceback
}
```

A C++ exception thrown in a function given with `def` becomes a Lube error, which the script can catch. `tests/embed/host.c` and `host.cpp` use every part of the API.

A program can also show Lube's windows itself, on a surface of its own (a game engine's texture, a phone's screen): with `src/lube_gfx.h`, `window()` opens on the host, which is given each frame's pixels and gives the script its events. The `gfx` plugin must be inside the program.

```c
#include "lube_gfx.h"

static void show(void *user, const uint32_t *pixels, int w, int h) { /* 0xAARRGGBB, rows top first */ }

LubeGfxHost host = {0};
host.present = show;               // also: pump (called for events), opened and closed (optional)
lube_gfx_set_host(&host);          // window() now opens here

LubeGfxEvent e = {LUBE_GFX_TOUCH_DOWN};
e.x = 10, e.y = 20;                // in the window's pixels: lube_gfx_fit(...) says where it is on the surface
lube_gfx_post(&e);                 // from any thread

LubeGfxEvent pad = {LUBE_GFX_PAD_AXIS};  // a game controller: PAD_CONNECTED (text: its name), PAD_DOWN and
pad.id = 0, pad.key = "left_x";          // PAD_UP (key: a button), PAD_AXIS (key: an axis, value: where it is),
pad.value = -0.5;                        // PAD_DISCONNECTED; id says which
lube_gfx_post(&pad);
```

`tests/embed/gfx_host.c` uses all of it.

## Phones: Android and iOS

`mobile/` has a template app for each: a Lube program that fills the screen, with touches as its input. Both run `mobile/game/main.lube` (a sample: tap to add a ball, with a sound from `sounds/pop.wav`), which also runs on a computer as it is (`lube mobile/game/main.lube`). Put your program there, as `main.lube`, with the files it reads beside it, in folders or not.

- **Android** (`mobile/android`): a NativeActivity, with no Java. Open the folder in Android Studio, or build it with Gradle (`gradle -p mobile/android assembleDebug`, or `installDebug` for a phone plugged in; a program in another folder: `-PlubeGame=/path/to/it`); it needs the NDK. The program's files are copied to the app's own storage, where it runs, when the app is new or has changed (so files the program writes there stay), and what it prints goes to the log (`adb logcat -s lube`). The back button is the `escape` key.
- **iOS** (`mobile/ios`): `cmake -G Xcode -S mobile/ios -B build/ios -DCMAKE_SYSTEM_NAME=iOS` (a program in another folder: `-DLUBE_GAME=/path/to/it`), then open `build/ios/LubeApp.xcodeproj` and run it (on a phone, after choosing a signing team). What it prints goes to Xcode's console.
- The program's window is shown as large as fits, in whole pixels where it can, with black around it: 180 by 320 fills a phone held upright. Touches come as `touch_down`, `touch_move` and `touch_up`, the first finger as the mouse too, so a program written for a mouse works with a finger. A hardware keyboard's keys come as keys, and game controllers as controllers. When the app goes to the background, the program gets `blur`, and on Android it waits there until the app comes back.
- Lube and its plugins are built into the app from `src/` (the templates find it from where they are: elsewhere, give its path with `-DLUBE_SRC=...`). Files, sockets, sound and graphics work as on a computer: sound with Audio Queue on iOS, and AAudio on Android 8 and later (in silence before that).

## How the interpreter works

```
source ──► lexer ──► tokens ──► parser ──► AST ──► resolver ──► compiler ──► bytecode ──► VM
```

Programs are compiled to bytecode every time they run, in memory: there is no build step. The VM runs a call to a Lube function in the same loop as its caller, with a frame of its own, so recursion doesn't use the C stack at all: a program can recurse 100,000 calls deep on a phone's small thread stack, and runaway recursion is a clean error. The loop has two tiers. The fast one keeps the VM's state in the processor's registers and runs the common instructions in their common cases. Everything else goes the long way, one instruction at a time, through the general code, and so does every instruction while the debugger is on. The fast tier is written once, in `src/interp_ops.h`, and compiled one of three ways: each instruction is a function that tail-calls the next (clang, where it has `musttail` and `preserve_none`), jumps through a table of labels (other GNU C compilers, gcc among them), or uses a plain C99 `switch` (any other compiler). All three give the same results; only their speed differs. Next to the tree-walking interpreter it replaced, it runs function calls about 1.8× faster and loops about 5× faster. The commonest operations have shortcuts that do what the general way does: `xs.push(v)` appends with no call when `v` runs none of the program's code, a store into an object or a map finds its key in one look, `==` on two strs compares them where they are, a package's function is called straight from the VM's loop, and a constructor or a callback with plain parameters starts without the checks it doesn't need. `tests/shortcuts.lube` prints the same with and without them. A str of up to 10 bytes is held in its 16-byte value, with no object behind it (longer ones, and the names a program uses, are objects): both are the same str to a program, equal and with the same hash, and a list that holds only Numbers, Bools and short strs is copied, sliced and freed as one block. `"...{x}..."` is put together from its pieces in place, a whole number's digits two at a time.

| file | role |
|---|---|
| `src/lexer.c` | Turns text into tokens. Emits end-of-statement tokens at line breaks where a statement can end. |
| `src/parser.c` | Recursive-descent parser that builds the syntax tree in an arena (freed in one step). Desugars `\|` into calls and parses string interpolations. |
| `src/resolver.c` | Runs before execution. Gives every local a slot in its function's frame and works out what each closure must copy. Enforces `let`/`var` and the closure rules, and catches misplaced `return` or `break`. |
| `src/compile.c` | Turns the resolved tree into bytecode: 32-bit instruction words, a source line for each, constants, and descriptors for calls, places, classes and `try` blocks. |
| `src/interp.c` | The VM and the runtime around it. Errors are ordinary return values, so every path releases what it owns. Also handles copy-on-write updates, lending with `&`, `on` blocks, imports and hot reload. |
| `src/interp_ops.h` | The VM's fast tier: the common instructions, written once and compiled by interp.c as tail calls, computed gotos or a `switch`. Whatever it doesn't handle, it hands to the general code with the VM's state stored back. |
| `src/value.c` | Text, lists (including typed lists), objects and maps (insertion-ordered hash maps, probed as Python's dicts are: whole-number keys hash to themselves, so keys 0, 1, 2... sit in a row), class instances, ranges, type names, equality and printing. |
| `src/integer.c` | Sized integers: conversions, overflow-checked arithmetic, exact comparisons across number types. |
| `src/memory.c` | Owner counts and freeing. Freeing never recurses, so even a list nested 100,000 levels deep is freed safely. |
| `src/builtins.c` | The standard library. |
| `src/reflect.c` | Reflection: classes, fields, methods, signatures and globals, from inside a program. |
| `src/bundle.c` | Building a program and its imports into a standalone executable, and running one. |
| `src/isolate.c` | Isolates and channels: starting isolates, copying values between their heaps, and noticing deadlocks. |
| `src/thread.c` | The operating system's threads (POSIX threads, or Win32): starting one, and a lock to wait on. |
| `src/main.c` | Command line and REPL. |

## Testing

Lube is written in C, and its plugins read files from anywhere (models, arrays, safetensors), so what keeps it safe is testing: the same tests, with the same expected output, on every system.

- **The suite.** `make test` runs `tests/*.lube` and `tests/text/*.lube` (strs of every length, held either way) and compares the output with the `.out` files next to them. `tests/cli.sh` covers the command line, the REPL, exit codes, tracebacks, the `LUBE_MAX_*` limits, and the programs in `tests/cli/stops.txt` that each stop with an error.
- **Checked against Python.** The plugins' expected outputs were checked against the Python libraries they match:
  - NumPy;
  - PyTorch, with every gradient also checked against finite differences;
  - scikit-learn;
  - Python's `math`, `cmath`, `statistics` and `random`, with the same random numbers digit for digit;
  - for `llm`, a NumPy model that computes the way llama.cpp does. `tests/models/make_models.py` makes the tiny GGUF models the tests load and prints what that model gives.
- **The same output everywhere.** Results that depend on the C library (`exp`, `sin`, float32's functions...) or on the system's LAPACK print rounded, and the decompositions run twice, with LAPACK and with array's own code, which must agree. The plugins are compiled without fused multiply-adds, so a sum comes out the same on ARM and x86, with either compiler.
- **Memory.** `make check` runs the suite on a build with AddressSanitizer and UndefinedBehaviorSanitizer. That build also fails if any object is still alive at exit, in the main program or in any isolate.
- **Threads.** `make tsan` runs it with ThreadSanitizer, which reports any data race: between isolates, and in the thread pool that `array`, `nn`, `learn` and `llm` share.
- **Coverage.** `make coverage` builds with clang's source-based coverage, runs the suite, and fails unless every line and every function of the interpreter and its plugins ran. The few defensive lines that no program can reach are marked `LCOV_EXCL_LINE` (or sit between `LCOV_EXCL_START` and `LCOV_EXCL_STOP`), and the report counts them.
- **In a browser.** `make browser` runs the web plugin's pages in headless Chrome, driven by `tests/browser/run.mjs` over the DevTools protocol. Each case starts an app on a free port (the example app, and `tests/browser/keys.lube`), uses it as a person would, and compares what the page shows, and its console, with a `.out` file. It checks that typing fast keeps focus, the caret and every letter, that list items moving with `key:` keep their elements, that a form clears after it is sent, that markup someone types stays text, and that the page shows when the server goes. The steps wait for `data-busy` to go, not for a fixed time. With `BROWSER_LUBE=./lube-check`, they run on the checking build, and the keys app, stopped by its `/stop` route, must end without a leak. It needs Node 22 or later and Chrome or Chromium (`CHROME=` names one). Without them it says so and passes, because it is not part of `make test`.
- **Fuzzing.** The interpreter has been fuzzed with tens of thousands of malformed and adversarial programs.

### On every pull request

GitHub Actions (`.github/workflows/ci.yml`) runs:

| job | what it runs |
|---|---|
| Linux, with gcc and with clang | the build with every warning an error, `make test`, `make check`, `make tsan`, `make browser` on both builds, a real window (X11 under Xvfb), a static binary |
| macOS | `make test`, `make check`, `make coverage`, a real Cocoa window, a universal binary |
| Windows (MSYS2, gcc) | `make test`, a real Win32 window, the release zip |
| Android | the template in `mobile/android`, built with Gradle, and its native part again with every warning an error |
| iOS | the template in `mobile/ios`, built for the simulator with warnings as errors |

Two compilers catch what one misses. gcc warns about values that might be used before they are set, where clang doesn't. The two also evaluate a call's arguments in opposite orders, which once hid a bug on macOS that Linux showed at once. Changes to the phone templates are also run by hand on the Android emulator and the iOS simulator.

### The soak: everything, for an hour

`tests/soak/run.sh` runs `tests/soak/soak.lube` for an hour (`run.sh SECONDS` for another length) and watches it. Each round runs ten workloads and times each one:

- the core language (recursion, text, lists, maps, classes, caught errors), Money, `math`, `cmath`, `statistics` and `random`;
- `array`: products, sorting, broadcasting, indexing, linear algebra, `.npy` and safetensors files;
- `nn`: training an MLP and a CNN, saving and loading the weights;
- `learn`: a forest, k-means, logistic regression, nearest neighbors, PCA, cross-validation;
- `llm`: loading a model, generating, tokenizing;
- isolates and channels, each isolate loading the plugins;
- 3,400 errors raised inside the plugins, where leaks like to hide;
- a thousand of the smallest operations, so that a leak of a few bytes each grows big enough to see.

Every round is seeded the same way, so its checksums must never change: one that does means memory went wrong somewhere.

- **What it records,** into `build/soak`:
  - every 5 seconds: resident memory, the load and the free disk;
  - on macOS, every minute: the physical footprint;
  - on macOS, every 10 minutes: `leaks` against the live process, which finds memory that nothing points to any more.
- **The summary.** `python3 tests/soak/analyze.py build/soak` reports the rounds, the checksums, each workload's speed at the start and at the end, and memory's trend.
- **Real models.** `llm` loads one of the tiny test models unless given real ones: `run.sh 3600 model.gguf [second.gguf]`.
- **A leak check of everything at once.** `LUBE=./lube-check tests/soak/run.sh 60` runs the soak on the sanitizer build. It found `sum()` leaking a total of Money per addition, which is now fixed and tested.
- **Against Python.** `tests/soak/soak.py` is the same workload in Python, with NumPy, PyTorch, scikit-learn and llama-cpp-python, for comparing speed: `python3 tests/soak/soak.py model.gguf 60 build/soak/tmp`.

**An hour on an M5** (10 cores, 16 GB), loading stories15M and SmolLM2-135M:

- **Rounds:** 7,019, and every checksum was the same.
- **Memory:** flat. Resident memory had a median of 372–378 MB in every 10 minutes, and the physical footprint peaked at 374 MB, trending slightly down. Most of it was the isolates': each interpreter reserved a 16 MB stack and a 16 MB type table, and malloc cleared that memory again for each new isolate. The interpreter now maps those from the system, so only the pages it touches count, and they go back when it ends: the same soak's footprint now peaks at 46 MB.
- **Leaks:** `leaks` found nothing at any of its six checks.
- **Speed:** a round took 445–456 ms (the median of each 10 minutes) for 50 minutes. The last 10 were slower only because the machine got busier: a fresh process started then was just as slow.

**Against Python.** The same workload in Python, run back to back with Lube for five minutes each, after the speed work below (the median of each, in ms; the machine was busy with other work at the time, the same for both):

| workload | Lube | Python | |
|---|---|---|---|
| core language | 8.4 | 9.5 | Lube 1.1× faster |
| Money (Python: `decimal`) | 5.3 | 13.5 | Lube 2.6× faster |
| `math`, `cmath`, `statistics`, `random` | 6.7 | 8.4 | Lube 1.3× faster |
| `array` (NumPy) | 13.5 | 27.2 | Lube 2.0× faster |
| `nn` (PyTorch) | 25.1 | 38.1 | Lube 1.5× faster |
| `learn` (scikit-learn, the forest on all cores) | 12.6 | 125.3 | Lube 10× faster |
| `llm` (llama-cpp-python), loading included | 89.4 | 181.3 | Lube 2.0× faster |
| isolates (Python: a process pool and threads) | 4.8 | 1,162.2 | Lube 242× faster |
| 3,400 errors | 1.8 | 37.4 | Lube 21× faster |
| tiny operations | 0.7 | 4.1 | Lube 5.8× faster |
| **a whole round** | **169** | **1,604** | **Lube 9.5× faster** (2.7× without the isolates) |

Before that work, Python won three of these: `array` (7.9× faster than Lube, almost all of it `eigh` by Jacobi's method), Money (2×) and `nn` (1.1×, GELU computing `erf` one element at a time).

### Against Python, case by case

`bench/vs_python` (see `bench/README.md`) times 1,100 cases in each language, every function at several sizes: the language, Money, `math`, `cmath`, `statistics` and `random`, and the plugins against NumPy, `numpy.linalg`, PyTorch, scikit-learn and llama-cpp-python. Python/Lube for each area, as the geometric mean of its cases (above 1, Lube is faster), on the M5: before the speed work, after its first pass (the libraries' kernels), and after its second (the interpreter, and what was left around the kernels):

| area | cases | before | first pass | second pass | where Python is still faster |
|---|---|---|---|---|---|
| core language | 114 | 1.10 | 1.07 | 1.52 | 37 |
| Money | 40 | 0.90 | 2.98 | 2.73 | 0 |
| `math`, `cmath`, `statistics`, `random` | 139 | 1.16 | 1.72 | 2.37 | 52 |
| `array` | 550 | 0.95 | 1.52 | 1.90 | 71 |
| linear algebra | 76 (68 before: 8 took too long) | 0.39 | 1.63 | (not measured again) | 7 |
| `nn` | 54 | 0.54 | 0.99 | 1.14 | 25 |
| `learn` | 90 | 5.27 | 6.51 | (not measured again) | 6 |
| `llm` | 17 | 3.12 | 4.58 | (not measured again) | 2 |
| isolates | 20 | 3.20 | 5.26 | (not measured again) | 4 |
| **all** | **1,100** | **1.09** | **1.76** | **2.13** | **204 (from 514, then 329)** |

(The second pass changed nothing in linear algebra, `learn` or isolates, and the machine was too busy with other work to time their threads again: their first-pass numbers stand, in the total too. In `llm` it changed generating, timed apart below.)

What the first pass changed: the decompositions call the system's LAPACK (as NumPy does), with a faster method of Lube's own where there is none; `median` and the percentiles select instead of sorting; masks, `unique`, reductions along an axis, `where`, `roll`, `repeat`, `nonzero` and indexing by positions got loops of their own; float32's activations and softmax in `nn` vectorize; Money converts a Number to cents without formatting it as text; the threads that `array`, `nn`, `learn` and `llm` share no longer wait for each other; and a package's function is found once per call site, not by name at each call.

What the second pass changed, each measured in instructions per operation (which other work on the machine does not change) against CPython 3.15's:

| operation | before | now | CPython |
|---|---|---|---|
| `xs.push(i)` | 642 | 279 | 445 |
| `m[i] = i`, 1,000 whole-number keys | 1,125 | 446 | 384 |
| `for w in words { if w == "..." { ... } }` | 419 | 195 | 161 |
| `Point(i, 2)` | 2,932 | 1,866 | 1,898 |
| `{a: i, b: 2}` | 2,030 | 1,264 | 1,673 |
| `math.sqrt(i)` | 959 | 652 | 838 |
| an index error, caught | 2,579 | 1,686 | 2,089 |

- `xs.push(v)` and `push(&xs, v)` append with no call when `v` runs none of the program's code (two instructions of the VM, with the call as before behind them for everything else).
- A store into an object or a map looks its key up once, not three times; maps are probed as Python's dicts are, with whole numbers hashing to themselves, so keys 0, 1, 2... fill the table in order; and the entries of small objects come from the interpreter's own free lists, not `malloc`.
- `==` on two strs compares them where they are, and a str written out is read where it is, as a Number is; `for x in xs` over a List has a path of its own.
- A package's function is called from the VM's loop; a constructor, and a function given to `map`, `filter` or `sort`, starts without the checks its plain parameters don't need.
- An error's message is put together without `printf`, which cost more than the rest of raising it.
- In `array`: `count_nonzero` and `all` of a mask go through 240 and 256 bytes at a time (`count_nonzero` 14 times as fast, 3 times NumPy's; `all` twice as fast), `x[positions]` uses an int64 Array of positions as it is (1.8 times as fast), and `max` and `min` along an axis compare twice an element, not three times (1.5 times). In `nn`: softmax's `exp` loop vectorizes (its rows 2.8 times as fast). None of these is written with intrinsics: where a loop already vectorized, writing it by hand gained 7% at most.
- In `llm`: a row's last blocks are read where they are (SmolLM2's matrices are 576 wide, so every row has some), and one vector's sums are no longer cleared a batch at a time: SmolLM2-135M generates 326 tokens a second on one thread, from 250 (llama.cpp: 369), and 357 on all of them, from 267 (llama.cpp: 357).
- `make pgo` builds `lube` with clang's profile-guided optimization: another 5–15% fewer instructions.

`tests/shortcuts.lube` holds each of the interpreter's shortcuts to what the general way gives: its expected output was printed by the interpreter before them.

A third pass was about text, and about what a program builds text from:

- A str of up to 10 bytes is held in its value, not in an object. `split`, `chars`, slices, `s[i]`, `str()`, `+`, interpolation and a plugin's results make short strs without asking for memory, and a map keyed by them hashes and compares two machine words.
- A list that holds no objects (Numbers, Bools, short strs) is copied and sliced with one `memcpy`, and freed as one block.
- `split`, `find`, `replace`, `count`, `in`, `upper` and `lower` go through text 8 bytes at a time, in plain C that gives the same result on every system.
- `"...{x}..."` puts its usual pieces (text written out, whole numbers, strs) together with no buffer of its own, and a whole number's digits are written two at a time, by `str()` and `print` too.

In instructions for one turn of a loop, against CPython 3.15's:

| operation | before | now | CPython |
|---|---|---|---|
| `"k{i}"` | 797 | 272 | 1,224 |
| `"item {i}"` (a result of more than 10 bytes) | 801 | 430 | 1,248 |
| `"{i},{i}"` | 1,150 | 568 | 2,187 |
| `str(i)` | 600 | 289 | 666 |

Over the 54 cases of the core language that run on one thread, Lube now runs about 0.7 times the instructions it did (geometric mean; 0.68 and 0.74 in two runs, the smallest cases varying from one to the next): `split` 0.07, a list's slice 0.09, `x in xs` 0.10, `replace` 0.45, interpolation 0.48, `in` on two strs 0.58, filling a map with str keys 0.85. Python/Lube by instructions went from 1.86 to between 2.5 and 2.7, and Lube does less work than CPython in 52 of the 54 (42 before); the two left are `map` and `filter` with a function. The table of areas above is by time and was not measured again.

`tests/text/` holds strs of every length around 10 bytes, made every way and meeting each other in every operation, with a plugin and a host program reading them through the C API (`tests/plugins/texts.c`): their expected output was printed by the interpreter before any of this.

Where Python is still faster, the cause is known:

- **`map` and `filter` with a function** (0.6–0.8×): each call of it enters the VM anew, where a Python comprehension runs in place. A loop that pushes is as fast as Python's.
- **Methods that change `this`:** 0.6–0.9×.
- **Calls that make an object each time:** `math.isclose(a, b, {rel_tol: ...})` builds its options, and each `cmath` result is a new Complex (0.4–0.65×). `math.comb` of large numbers and `math.dist` of long lists (0.3–0.6×) read their arguments one by one.
- **`nn` on small batches:** GELU on a 64×1024 batch (0.4×: its float32 kernel does without fused multiply-adds, so that every system gives the same bits) and softmax (0.6×, from 0.1×); on big ones Lube is as fast or faster. Loading a big state dict (0.2×): PyTorch's safetensors maps the file, where Lube reads it.
- **`llm`:** reading a prompt (0.55× llama.cpp's for 128 tokens through SmolLM2, timed one after the other: the table's cases were timed while llama.cpp's threads had other work to wait for), because Lube promises the same logits on every system and for a prompt read at once or a token at a time, which rules out llama.cpp's reordered matrix kernels for batches.
- **Starting an isolate** (0.2× a Python thread): each one is a fresh interpreter that runs the program's declarations again; a pool of them (`parallel_map`) is 3–5 times as fast as Python's.
- **`from_bytes`:** NumPy's `frombuffer` does not copy, and Lube's Array owns its memory.
