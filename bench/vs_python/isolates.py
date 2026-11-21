"""isolates.py: threads and queues, multiprocessing and ProcessPoolExecutor (isolates.lube does the same with Lube's
isolates and channels): starting and joining, sharing work out over every core, and sending values between them.
Each case's name says what Lube's isolates are measured against here. Processes start the platform's default way
(spawn on macOS); a persistent pool is made before its cases are timed, a new pool inside them."""
from harness import bench  # (first: see harness.py)
import multiprocessing as mp
import os
import queue
import threading
from concurrent.futures import ProcessPoolExecutor, ThreadPoolExecutor

CPUS = os.cpu_count()


def noop(x):
    return x


def fib(n):
    if n < 2:
        return n
    return fib(n - 1) + fib(n - 2)


def echo(jobs, results):  # (threads and processes alike: until None)
    while True:
        v = jobs.get()
        if v is None:
            return
        results.put(v * 2)


def summer(jobs, results):
    while True:
        xs = jobs.get()
        if xs is None:
            return
        results.put(sum(xs))


def main():
    # ---------- start and join ----------
    def spawn_thread():
        out = []
        t = threading.Thread(target=lambda: out.append(noop(1)))
        t.start()
        t.join()
        return out[0]
    bench("spawn_join_vs_thread", 1, spawn_thread)

    def spawn_process():
        p = mp.Process(target=noop, args=(1,))
        p.start()
        p.join()
        return 1
    bench("spawn_join_vs_process", 1, spawn_process)

    for n in [10, 100]:
        def many():
            res = [0] * n

            def run(i):
                res[i] = fib(15)
            ts = [threading.Thread(target=run, args=(i,)) for i in range(n)]
            for t in ts:
                t.start()
            for t in ts:
                t.join()
            return sum(res)
        bench("spawn_join_many_vs_threads", n, many)

    # ---------- work over every core: n tasks of fib(k) ----------
    pool = ProcessPoolExecutor(CPUS)
    list(pool.map(noop, range(CPUS * 2)))  # (its processes started)
    tpool = ThreadPoolExecutor(CPUS)
    for count, k in [(100, 15), (40, 24)]:
        tasks = [k - i % 3 for i in range(count)]
        size = f"{count}xfib{k}"
        bench("map_serial", size, lambda: list(map(fib, tasks)))
        bench("parallel_map_vs_pool", size, lambda: list(pool.map(fib, tasks)))

        def new_pool():
            with ProcessPoolExecutor(CPUS) as ex:
                return list(ex.map(fib, tasks))
        bench("parallel_map_vs_new_pool", size, new_pool)
        bench("parallel_map_vs_threads", size, lambda: list(tpool.map(fib, tasks)))
    pool.shutdown()
    tpool.shutdown()

    # ---------- channels: n numbers to a worker and back ----------
    for n in [1000, 100000]:
        def roundtrip():
            jobs, results = queue.Queue(), queue.Queue()
            w = threading.Thread(target=echo, args=(jobs, results))
            w.start()
            for i in range(n):
                jobs.put(i)
            jobs.put(None)
            out = [results.get() for _ in range(n)]
            w.join()
            return sum(out)
        bench("channel_roundtrip_vs_thread_queue", n, roundtrip)

        def pool4():
            jobs, results = queue.Queue(), queue.Queue()
            ws = [threading.Thread(target=echo, args=(jobs, results)) for _ in range(4)]
            for w in ws:
                w.start()
            for i in range(n):
                jobs.put(i)
            for w in ws:
                jobs.put(None)
            out = [results.get() for _ in range(n)]
            for w in ws:
                w.join()
            return sum(out)
        bench("channel_pool4_vs_thread_queue", n, pool4)

    for n in [1000, 10000]:
        def roundtrip_process():
            jobs, results = mp.Queue(), mp.Queue()
            w = mp.Process(target=echo, args=(jobs, results))
            w.start()
            for i in range(n):
                jobs.put(i)
            jobs.put(None)
            out = [results.get() for _ in range(n)]
            w.join()
            return sum(out)
        bench("channel_roundtrip_vs_process_queue", n, roundtrip_process)

    # ---------- sending a list of n numbers to a running worker, and its sum back ----------
    jobs, results = mp.Queue(), mp.Queue()
    worker = mp.Process(target=summer, args=(jobs, results))
    worker.start()
    for n in [1000, 100000]:
        xs = list(range(n))

        def send_list():
            jobs.put(xs)
            return results.get()
        bench("send_list_vs_process_queue", n, send_list)
    jobs.put(None)
    worker.join()


if __name__ == "__main__":
    main()
