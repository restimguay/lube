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
- **Values can never form cycles.** A value can't contain itself (`box.self = box` stores a *copy* of `box`), closures capture copies (see below), and class instances are values too. Without cycles, a simple owner count is enough: when the last owner lets go, the memory is freed immediately.
- **Memory is freed deterministically.** Values are freed at the end of a block, when a variable is overwritten, or when a function returns. There's no collector running in the background, no pauses, and no leaks.
- **Iteration can't be broken.** A loop over a list keeps seeing the list as it was when the loop started, even if the body changes the variable.

Lube programs can't corrupt memory: there are no pointers, every index is bounds-checked, integer arithmetic is overflow-checked, and nothing is ever freed by hand. Extreme input gets a clear error instead of a crash. That covers numbers like `1e300` or NaN used as sizes, code nested thousands of levels deep, and runaway recursion. A string can hold at most 1 GB and a list, object or map at most 268 million items, so one expression can't claim all of the machine's memory. To run untrusted scripts with less, set `LUBE_MAX_BYTES` (the longest string, and the largest source file) and `LUBE_MAX_ITEMS` (the most items in one list, object or map). They can lower the limits but never raise them. A program that goes over a limit stops with `lube: out of memory`.

The interpreter itself is written in C, so its safety rests on testing. `make check` runs the whole suite under AddressSanitizer and UndefinedBehaviorSanitizer and verifies at exit that every object was freed. `make coverage` checks that the suite runs every line and every function of the interpreter. The interpreter has also been fuzzed with tens of thousands of malformed and adversarial programs.

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
| plugins' types | `File`, `Socket`, `Sound`, `Voice`: see Plugins below |

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

Every value has a type, and `type(x)` gives its name: `str`, `String`, `Number`, `Bool`, `Object`, `List`, `Map`, `Function`, `Range`, the sized integers such as `int32`, typed lists such as `String[]`, typed maps such as `Map[str, Number]`, and class names. `x is T` checks a type, including a class and its parents. Type names do three things:

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
| arithmetic | `+` `-`, then `*` `/` `%`, then unary `-` `!`, then `**` |

The pipe passes its left side as the first argument: `x | f(y)` is `f(x, y)`, and `x | f` is `f(x)`.

`%` follows the sign of the divisor (`-7 % 3` is `2`), and dividing by zero is an error.

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

A **map** is like an object, but its keys can be any value, including lists and objects, compared by contents. `Map[K, V]` makes one that checks its key and value types:

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

### Plugins: files, sockets, sound and graphics

Some of Lube comes as plugins: packages written in C, loaded when a program imports them, so a program that uses none of them doesn't load them. `make` builds the four that come with Lube into `plugins/` next to `lube`: `fs` for files and folders, `net` for sockets, `audio` for sound and `gfx` for graphics. They are imported as any package is:

```lube
import fs.*                // open, exists, list_dir...: their names, bare
import net                 // net.tcp_listen(7777), net.Socket
import audio as sfx        // sfx.Sound("jump.wav")
import gfx.*               // window(320, 180), image(16, 16), rgb(255, 128, 0)...
```

A plugin's types (`File`, `Socket`, `Sound`, `Voice`, `Image`, `Window`) work as classes do: `x is Socket`, `fn send_all(s: Socket)`, `class_of(x)`, `methods(x)`. Their objects are the plugin's: a copy shares the one file or socket, and they stay in the isolate that made them. Anyone can write a plugin: see Writing a plugin below.

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
- Windows need no library to build Lube: they use what the system has, loaded when the first window opens. That is Cocoa on macOS, X11 on Linux and the Raspberry Pi, and Win32 on Windows. With `LUBE_GFX=none` a window is shown nowhere (on a server, or in tests: everything works but the screen), and the static Linux binary from the releases has no windows (it can still draw images and save them).

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
- `src/lube_plugin.h` describes every function, and Lube's own plugins (`src/plugins/fs.c`, `net.c`, `audio.c` and `gfx.c`) are complete examples.

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

All of these are globals, and all of them can be called in method style; the rows for `fs`, `net` and `audio` are those plugins' names, after their import (see Plugins). The methods of `Bytes` and of the plugins' types, such as `b.pack(...)` and `f.read()`, are methods only.

| area | functions |
|---|---|
| change a list or object in place (take `&`) | `push(&xs, ...)`, `pop(&xs)`, `insert(&xs, i, x)`, `remove(&xs, i)`, `delete(&obj, key)` (returns whether the key was there), `clear(&x)`; method style needs no `&`: `xs.push(4)` |
| output and input | `print(...)`, `write(...)` (no newline), `input(prompt?)` (an error at end of input), `read_file(path)`, `write_file(path, text)`, `append_file(path, text)`, `exit(code?)` |
| types | `str(x)`, `String(x)`, `Number(x, fallback?)`, `Bool(x)`, `Object(x)`, `Map(x)`, `List(x)`, `Function(f)`, `Range(...)`, plus the sized integers; `type(x)` gives the type's name; the lowercase `num`, `int`, `bool` and `list` still work; `repr(x)` |
| sized integers | `int8`, `int16`, `int32`, `int64`, `uint8`, `uint16`, `uint32`, `uint64`; each takes an optional value and defaults to 0 |
| lists | `len`, `slice(x, start, end?)`, `reverse`, `sort(xs, compare?)`, `contains`, `index_of`, `join(xs, sep?)`, `list(x)`, `zip(a, b)` |
| objects | `keys`, `values`, `items`, `has(obj, key)`, `get(obj, key, default?)` (a missing key without a default is an error) |
| functional | `map`, `filter`, `each`, `reduce(xs, f, start?)`, `find(xs, f, default?)`, `find_index` (-1 if none), `any(xs, f?)`, `all(xs, f?)`, `sum`, `min`, `max`, `range(end)` / `range(start, end, step?)` |
| strings | `upper`, `lower`, `trim`, `split(s, sep?)`, `replace(s, old, new)`, `starts_with`, `ends_with`, `chars`, `ord`, `chr`, `pad_left(x, width, fill?)`, `pad_right`, `fixed(n, digits?)` |
| math | `abs`, `floor`, `ceil`, `round(x, digits?)`, `sqrt`, `pow`, `sin`, `cos`, `tan`, `atan2`, `log(x, base?)`, `exp`, `PI`, `INF` |
| random | `random()`, `random_int(lo, hi)`, `choice(xs)`, `shuffle(xs)`, `seed(n)` |
| reflection | `class_of`, `parent_of`, `fields`, `methods`, `signature(f)` / `signature(class, method)`, `get_field`, `set_field(&x, name, value)`, `call_method(x, name, ...)`, `has_method`, `classes()`, `find_class(name)`, `globals()` (see Reflection) |
| concurrency | `spawn(f, args...)`, `iso.join()`, `channel(n?)`, `send`, `receive`, `try_receive(ch, default)`, `take(ch, n)`, `close`, `cores()`, `parallel_map(xs, f)` (see Concurrency) |
| bytes | `Bytes(x?)`, `len`, `push(&b, ...)`, `+`, `in`; methods `b.pack(format, n, at?)`, `b.unpack(format, at?)`, `b.decode()`, `b.hex()`, `b.slice(from, to?)` (see Bytes and time) |
| `audio` (plugin) | `Sound(path or bytes)`, `Sound(samples, rate?, channels?)`, `stop_all()`, `set_volume(x)`, `device()`; methods `s.play(volume?, pan?)`, `s.loop(volume?, pan?)`, `s.duration()`, `s.rate()`, `s.channels()`, `s.samples()`, `s.to_wav()`; `v.stop()`, `v.is_playing()`, `v.set_volume(x)`, `v.set_pan(x)`, `v.set_speed(x)` (see Sound) |
| `net` (plugin) | `tcp_listen(port, host?)`, `tcp_connect(host, port, seconds?)`, `udp_open(port?, host?)`, `poll(sockets, seconds?)`; methods `s.accept(seconds?)`, `s.receive(seconds?)`, `s.send(data)`, `s.packets(seconds?)`, `s.send_to(host, port, data)`, `s.is_open()`, `s.port()`, `s.peer()`, `s.close()` (see Sockets) |
| `fs` (plugin) | `open(path, mode?)`; methods `f.read(n?)`, `f.read_line()`, `f.write(x)`, `f.seek(pos, from?)`, `f.tell()`, `f.size()`, `f.eof()`, `f.flush()`, `f.close()`; `read_bytes`, `write_bytes`, `exists`, `is_dir`, `list_dir`, `make_dir`, `remove_file`, `rename_file`, `file_size` |
| misc | `assert(cond, message?)`, `clock()` (CPU seconds), `time()` (Unix time), `now()` (a steady clock, for timing), `sleep(seconds)`, `hot_reload()` (see Hot reload), `args` |

Everything outside the first row returns a new value and leaves its arguments alone; `sort` and `reverse` return new lists. `map`, `filter` and `each` also pass the index when the function takes two parameters: `xs.map(fn(x, i) => ...)`. A `sort` comparator can return a number (negative means `a` goes first) or a bool (`true` means `a` goes first).

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
```

`tests/embed/gfx_host.c` uses all of it.

## Phones: Android and iOS

`mobile/` has a template app for each: a Lube program that fills the screen, with touches as its input. Both run `mobile/game/main.lube` (a sample: tap to add a ball), which also runs on a computer as it is (`lube mobile/game/main.lube`). Put your program there, as `main.lube`, with the files it reads beside it.

- **Android** (`mobile/android`): a NativeActivity, with no Java. Open the folder in Android Studio, or build it with Gradle (`gradle -p mobile/android assembleDebug`, or `installDebug` for a phone plugged in); it needs the NDK. The program's files are copied to the app's own storage, where it runs, and what it prints goes to the log (`adb logcat -s lube`). The back button is the `escape` key.
- **iOS** (`mobile/ios`): `cmake -G Xcode -S mobile/ios -B build/ios -DCMAKE_SYSTEM_NAME=iOS`, then open `build/ios/LubeApp.xcodeproj` and run it (on a phone, after choosing a signing team). What it prints goes to Xcode's console.
- The program's window is shown as large as fits, in whole pixels where it can, with black around it: 180 by 320 fills a phone held upright. Touches come as `touch_down`, `touch_move` and `touch_up`, the first finger as the mouse too, so a program written for a mouse works with a finger. A hardware keyboard's keys come as keys. When the app goes to the background, the program gets `blur`, and on Android it waits there until the app comes back.
- Lube and its plugins are built into the app from `src/` (the templates find it from where they are: elsewhere, give its path with `-DLUBE_SRC=...`). Files, sockets and graphics work as on a computer. Sound works on iOS; on Android it plays in silence for now.

## How the interpreter works

```
source ──► lexer ──► tokens ──► parser ──► AST ──► resolver ──► compiler ──► bytecode ──► VM
```

Programs are compiled to bytecode every time they run, in memory: there is no build step. The VM runs a call to a Lube function in the same loop as its caller, with a frame of its own, so recursion doesn't use the C stack at all: a program can recurse 100,000 calls deep on a phone's small thread stack, and runaway recursion is a clean error. Next to the tree-walking interpreter it replaced, it runs function calls about 1.8× faster and loops about 5× faster.

| file | role |
|---|---|
| `src/lexer.c` | Turns text into tokens. Emits end-of-statement tokens at line breaks where a statement can end. |
| `src/parser.c` | Recursive-descent parser that builds the syntax tree in an arena (freed in one step). Desugars `\|` into calls and parses string interpolations. |
| `src/resolver.c` | Runs before execution. Gives every local a slot in its function's frame and works out what each closure must copy. Enforces `let`/`var` and the closure rules, and catches misplaced `return` or `break`. |
| `src/compile.c` | Turns the resolved tree into bytecode: 32-bit instruction words, a source line for each, constants, and descriptors for calls, places, classes and `try` blocks. |
| `src/interp.c` | The VM and the runtime around it. Errors are ordinary return values, so every path releases what it owns. Also handles copy-on-write updates, lending with `&`, `on` blocks, imports and hot reload. |
| `src/value.c` | Text, lists (including typed lists), objects and maps (insertion-ordered hash maps), class instances, ranges, type names, equality and printing. |
| `src/integer.c` | Sized integers: conversions, overflow-checked arithmetic, exact comparisons across number types. |
| `src/memory.c` | Owner counts and freeing. Freeing never recurses, so even a list nested 100,000 levels deep is freed safely. |
| `src/builtins.c` | The standard library. |
| `src/reflect.c` | Reflection: classes, fields, methods, signatures and globals, from inside a program. |
| `src/bundle.c` | Building a program and its imports into a standalone executable, and running one. |
| `src/isolate.c` | Isolates and channels: starting isolates, copying values between their heaps, and noticing deadlocks. |
| `src/thread.c` | The operating system's threads (POSIX threads, or Win32): starting one, and a lock to wait on. |
| `src/main.c` | Command line and REPL. |

Testing:

- `make test` runs `tests/*.lube` and compares the output with the `.out` files next to them.
- `make check` runs the same suite on a build with AddressSanitizer and UndefinedBehaviorSanitizer. That build also fails if any object is still alive at exit, in the main program or in any isolate.
- `make tsan` runs it with ThreadSanitizer, which reports any data race between isolates.
- `tests/cli.sh` covers the command line, the REPL, exit codes, tracebacks, the `LUBE_MAX_*` limits, and the programs in `tests/cli/stops.txt` that each stop with an error.
- `make coverage` builds with clang's source-based coverage, runs the suite, and fails unless every line and every function of the interpreter ran. The few defensive lines that no program can reach are marked `LCOV_EXCL_LINE` (or sit between `LCOV_EXCL_START` and `LCOV_EXCL_STOP`), and the report counts them.
