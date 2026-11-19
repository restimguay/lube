# Benchmarks

Small programs for timing the interpreter (`time ./lube bench/fib.lube`).
`p_serial.lube` and `p_parallel.lube` do the same work with `map` and with
`parallel_map`, to see what the isolates gain on this machine's cores.
