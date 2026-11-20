# Benchmarks

Small programs for timing the interpreter (`time ./lube bench/fib.lube`).
`p_serial.lube` and `p_parallel.lube` do the same work with `map` and with
`parallel_map`, to see what the isolates gain on this machine's cores.

## Lube against Rust

`vs_rust/` has the same programs in Lube and in Rust: calls (`fib`), loops,
floating point and fields (`nbody`), allocation (`trees`), list indexing
(`sieve`), hash maps, strings, method calls, sorting, and starting up. Each
pair prints the same output.

```sh
python3 bench/vs_rust/run.py              # every pair: CPU time, peak memory, Lube/Rust
python3 bench/vs_rust/run.py fib nbody    # some of them
python3 bench/vs_rust/run.py --base OLD   # this lube against another build of it
python3 bench/vs_rust/run.py --native     # each program built with lube --build --native
python3 bench/vs_rust/run.py --native --fast  # ... with --native --fast (wrapping integers, reordered sums)
```

Rust is built with `rustc -C opt-level=3`. `--base` runs the two lubes
alternately; on macOS it counts CPU cycles and instructions, which other
programs running at the same time barely affect.
