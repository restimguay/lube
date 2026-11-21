"""core.py: the language itself (core.lube does the same in Lube): calls, loops, text, dicts, lists, sorting,
closures, classes and methods, exceptions. The size is the number of items or iterations. Python is written
as Python is written: list comprehensions for map and filter, key= for sorting."""
from harness import bench, bench_chk  # (first: see harness.py)
import functools
import random



def fib(n):
    if n < 2:
        return n
    return fib(n - 1) + fib(n - 2)


def add(a, b):
    return a + b


class Point:
    def __init__(self, x=0, y=0):
        self.x = x
        self.y = y

    def move(self, dx):
        self.x += dx

    def norm2(self):
        return self.x * self.x + self.y * self.y


class Point3(Point):
    def __init__(self, x, y, z):
        super().__init__(x, y)
        self.z = z

    def norm2(self):
        return super().norm2() + self.z * self.z


def weighted(xs):
    s = 0
    for i, x in enumerate(xs):
        s += (i + 1) * x
    return s


def weighted_len(xs):
    s = 0
    for i, x in enumerate(xs):
        s += (i + 1) * len(x)
    return s


def main():
    for n in [15, 20, 25]:
        bench("fib_recursive", n, lambda: fib(n))

    for n in [1000, 100000, 1000000]:
        def loop_for_range():
            s = 0
            for i in range(n):
                s += i * i
            return s
        bench("loop_for_range", n, loop_for_range)

        def loop_while():
            s = 0
            i = 0
            while i < n:
                s += i
                i += 1
            return s
        bench("loop_while", n, loop_while)

    for n in [1000, 100000]:
        def loop_float_math():
            x = 0.5
            s = 0
            for i in range(n):
                x = x * 1.0000001 + 0.25
                s += x / (i + 1)
            return s
        bench("loop_float_math", n, loop_float_math)

        def call_function():
            s = 0
            for i in range(n):
                s = add(s, i)
            return s
        bench("call_function", n, call_function)

        adder = lambda a, b: a + b  # noqa: E731

        def call_closure():
            s = 0
            for i in range(n):
                s = adder(s, i)
            return s
        bench("call_closure", n, call_closure)

        def make_closures():
            fs = []
            for i in range(n):
                fs.append(lambda x, i=i: x + i)
            return len(fs)
        bench("make_closures", n, make_closures)

    # ---------- text ----------
    random.seed(1)
    for n in [1000, 100000]:
        def str_concat():
            s = ""
            for i in range(n):
                s += "ab"
            return len(s)
        bench("str_concat", n, str_concat)

        def str_interpolate():
            total = 0
            for i in range(n):
                total += len(f"w{i}-{i % 13}")
            return total
        bench("str_interpolate", n, str_interpolate)

        def str_from_number():
            total = 0
            for i in range(n):
                x = i * 0.5
                total += len(str(int(x)) if x == int(x) else repr(x))  # (Lube prints 2.0 as 2)
            return total
        bench("str_from_number", n, str_from_number)

        parts = [str(i) for i in range(n)]
        text = ",".join(parts)
        bench("str_join", n, lambda: len(",".join(parts)))
        bench("str_split", n, lambda: len(text.split(",")))
        bench("str_replace", n, lambda: len(text.replace("1", "one")))
        bench("str_upper", n, lambda: len(text.upper()))

        def str_to_number():
            s = 0
            for p in parts:
                s += float(p)
            return s
        bench("str_to_number", n, str_to_number)

        def str_contains():
            c = 0
            for p in parts:
                if "99" in p:
                    c += 1
            return c
        bench("str_contains", n, str_contains)

        words = [f"w{i % 97}-{i % 13}" for i in range(n)]

        def str_compare_eq():
            c = 0
            for w in words:
                if w == "w5-5":
                    c += 1
            return c
        bench("str_compare_eq", n, str_compare_eq)

    # ---------- dicts ----------
    for n in [1000, 100000, 1000000]:
        ks = [f"k{i}" for i in range(n)]

        def dict_insert_str():
            d = {}
            for k in ks:
                d[k] = 1
            return len(d)
        bench("dict_insert_str", n, dict_insert_str)

    for n in [1000, 100000]:
        ks = [f"k{i}" for i in range(n)]
        d = {k: i for i, k in enumerate(ks)}

        def dict_lookup_str():
            s = 0
            for k in ks:
                s += d[k]
            return s
        bench("dict_lookup_str", n, dict_lookup_str)

        def dict_get_default():
            s = 0
            for i in range(n):
                s += d.get("nope", 1)
            return s
        bench("dict_get_default", n, dict_get_default)

        def dict_insert_int():
            e = {}
            for i in range(n):
                e[i] = i
            return len(e)
        bench("dict_insert_int", n, dict_insert_int)

        def dict_count_words():
            counts = {}
            for k in ks:
                w = k[0:3]
                counts[w] = counts.get(w, 0) + 1
            return len(counts)
        bench("dict_count_words", n, dict_count_words)

        def dict_iterate():
            s = 0
            for k, v in d.items():
                s += v
            return s
        bench("dict_iterate", n, dict_iterate)

        def map_tuple_keys():
            seen = {}
            for i in range(n):
                seen[(i % 50, i % 7)] = i
            return len(seen)
        bench("map_tuple_keys", n, map_tuple_keys)

        def object_create():
            s = 0
            for i in range(n):
                o = {"x": i, "y": 2, "name": "p"}
                s += o["x"]
            return s
        bench("object_create", n, object_create)

    # ---------- lists ----------
    for n in [1000, 100000, 1000000]:
        def list_append():
            xs = []
            for i in range(n):
                xs.append(i)
            return len(xs)
        bench("list_append", n, list_append)

    for n in [1000, 100000]:
        xs = list(range(n))

        def list_index_sum():
            s = 0
            for i in range(n):
                s += xs[i]
            return s
        bench("list_index_sum", n, list_index_sum)

        def list_iterate():
            s = 0
            for x in xs:
                s += x
            return s
        bench("list_iterate", n, list_iterate)

        def list_set_item():
            ys = [0] * n
            for i in range(n):
                ys[i] = i
            return weighted(ys)
        bench("list_set_item", n, list_set_item)

        bench("list_map", n, lambda: [x * 2 for x in xs])
        bench("list_filter", n, lambda: [x for x in xs if x % 3 == 0])
        bench("list_reduce", n, lambda: functools.reduce(lambda a, b: a + b, xs, 0))
        bench("list_sum_builtin", n, lambda: sum(xs))
        bench("list_slice", n, lambda: xs[1:n - 1])
        bench("list_reverse", n, lambda: weighted(xs[::-1]))
        bench("list_contains", n, lambda: (n - 1) in xs)

        def list_copy_on_write():  # (Lube's copy is lazy; Python's is the copy itself)
            ys = xs.copy()
            ys[0] = 5
            return ys[0]
        bench("list_copy_on_write", n, list_copy_on_write)

        def list_of_lists():
            grid = []
            for i in range(n):
                grid.append([i, i + 1])
            return len(grid)
        bench("list_of_lists", n, list_of_lists)

    # ---------- sorting ----------
    for n in [1000, 100000, 1000000]:
        random.seed(2)
        nums = [random.random() for i in range(n)]
        bench_chk("sort_numbers", n, lambda: sorted(nums), weighted)

    for n in [1000, 100000]:
        random.seed(3)
        nums = [random.random() for i in range(n)]
        ints = [random.randint(0, 1000000) for i in range(n)]
        strs = [f"s{i}" for i in ints]
        bench_chk("sort_ints", n, lambda: sorted(ints), weighted)
        bench_chk("sort_strings", n, lambda: sorted(strs), weighted_len)
        bench_chk("sort_custom_order", n, lambda: sorted(nums, key=lambda x: -x), weighted)
        people = [{"name": strs[i], "age": ints[i] % 100} for i in range(n)]
        bench_chk("sort_objects_by_field", n, lambda: sorted(people, key=lambda p: p["age"]),
                  lambda r: weighted([p["age"] for p in r]))

    # ---------- classes and methods ----------
    for n in [1000, 100000]:
        def class_construct():
            s = 0
            for i in range(n):
                p = Point(i, 2)
                s += p.x
            return s
        bench("class_construct", n, class_construct)

        def method_call():
            p = Point(1, 2)
            s = 0
            for i in range(n):
                s += p.norm2()
            return s
        bench("method_call", n, method_call)

        def method_mutate():
            p = Point(0, 0)
            for i in range(n):
                p.move(1)
            return p.x
        bench("method_mutate", n, method_mutate)

        def field_read_write():
            p = Point(0, 0)
            for i in range(n):
                p.y = p.x + i
            return p.y
        bench("field_read_write", n, field_read_write)

        def method_inherited_super():
            p = Point3(1, 2, 3)
            s = 0
            for i in range(n):
                s += p.norm2()
            return s
        bench("method_inherited_super", n, method_inherited_super)

        def list_of_instances():
            ps = []
            for i in range(n):
                ps.append(Point(i, i))
            s = 0
            for p in ps:
                s += p.x
            return s
        bench("list_of_instances", n, list_of_instances)

    # ---------- exceptions ----------
    for n in [1000, 100000]:
        def try_no_throw():
            c = 0
            for i in range(n):
                try:
                    c += 1
                except Exception:
                    c -= 1
            return c
        bench("try_no_throw", n, try_no_throw)

        def throw_catch():
            c = 0
            for i in range(n):
                try:
                    raise ValueError("bad")
                except ValueError:
                    c += 1
            return c
        bench("throw_catch", n, throw_catch)

        def catch_index_error():
            xs = [1, 2]
            c = 0
            for i in range(n):
                try:
                    c += xs[5]
                except IndexError:
                    c += 1
            return c
        bench("catch_index_error", n, catch_index_error)

        def catch_in_callee():
            c = 0
            for i in range(n):
                try:
                    c += fib(-1) + [1][3]
                except IndexError:
                    c += 1
            return c
        bench("catch_in_callee", n, catch_in_callee)

    # ---------- bytes ----------
    for n in [1000, 100000]:
        def bytes_build():
            b = bytearray()
            for i in range(n):
                b.append(i % 256)
            return len(b)
        bench("bytes_build", n, bytes_build)


if __name__ == "__main__":
    main()
