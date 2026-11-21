# Lube against Python

The same computations in Lube and in Python, case by case, at several sizes,
to find where Lube is slower than Python and its C libraries. Each area has a
Lube script and a Python script:

| area | Lube | Python |
|---|---|---|
| `core` | the language: calls, loops, text, objects and maps, lists, sorting, closures, classes, exceptions | the language |
| `money` | `Money` | `decimal`, rounding half up to the cent |
| `numbers` | `math`, `cmath`, `statistics`, `random` | the modules of those names |
| `array` | `array`: making Arrays, every elementwise function, masks, reductions, sorting, indexing, shapes, files | numpy |
| `linalg` | `array`'s linear algebra, n = 10 to 1000 | `numpy.linalg` |
| `nn` | `nn`: layers, activations, losses, backward, optimizers, training steps | PyTorch |
| `learn` | `learn`: every model's fit and predict, model selection, metrics | scikit-learn |
| `llm` | `llm`: loading, tokenizing, prompts, generating | llama-cpp-python |
| `isolates` | `spawn`, channels, `parallel_map` | threads and queues, multiprocessing, ProcessPoolExecutor |

```sh
python3 bench/vs_python/run.py                    # every area (about 20 minutes on 10 cores)
python3 bench/vs_python/run.py array linalg       # some of them
python3 bench/vs_python/run.py core --filter sort # the cases whose name has "sort" in it
python3 bench/vs_python/run.py --json new.json --md new.md   # keep the results
python3 bench/vs_python/run.py --compare old.json # each case's Lube time, before and after
python3 bench/vs_python/run.py --losers old.json --json new.json  # run again the cases Python won
python3 bench/vs_python/run.py llm --model stories15M-q8_0.gguf   # GGUF files for llm ($BENCH_MODELS)
```

`--lube` (or `$LUBE`) picks the lube, `./lube` by default, and `--python` (or
`$PYTHON`) the Python, `python3` by default; it needs numpy, torch,
safetensors, scikit-learn (with scipy) and llama-cpp-python for every area.
Without `--model`, `llm` loads the tiny test model in `tests/models`.

Each case is called once (its checksum comes from that call), then calibrated
so that one measurement takes at least 50 ms (`--min`), then measured 5 times
(`--repeats`; 3 times when a call takes over a second, once over 5 s). The
best measurement counts, as seconds per call; both sides time it the same way
(`harness.lube` and `harness.py`). The table shows Python/Lube: above 1, Lube
is faster.

Each script prints `case <TAB> size <TAB> seconds <TAB> checksum`. The
checksum is the sum of the result and the sum of its absolute values; the two
sides agree when both are within 1e-4 of the larger (a case can loosen it:
iterative solvers 1e-2, forests 0.05). A disagreement shows as `DIFF`.
Both sides draw the same random numbers: `array` gives numpy's for the same
seed, `random` Python's, and `nn`'s weights are drawn with numpy's and given
to both. Sampled text (`generate_sampled`) can't agree, and isn't compared.

Things to know when reading it:

- Lube's `array`, `nn`, `learn` and `llm` use every core for big inputs;
  numpy's elementwise functions use one. PyTorch, BLAS and llama.cpp use
  their own thread counts, and scikit-learn's forests are given `n_jobs=-1`.
- A linear algebra case at n = 1000 whose time at 300 promises more than
  10 s (scaled by n^3.5) is skipped, with the estimate.
- Python is written as Python is written: comprehensions for `map` and
  `filter`, `key=` for sorting.
- The machine should be quiet: other programs make both sides slower, and
  multithreaded code (llama.cpp's above all) much slower. When it cannot be,
  count instructions instead of time for single-threaded code:
  `/usr/bin/time -l ./lube x.lube` (macOS) or `perf stat -e instructions`
  (Linux) gives the same number however busy the machine is.
