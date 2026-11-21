# soak.py MODEL SECONDS TMPDIR [MODEL2] [ONLY]: soak.lube's workloads in Python, for comparing speed, with numpy, PyTorch,
# scikit-learn and llama-cpp-python; the same line per round:  R <round> <elapsed s> <ms of each...> chk=...
import sys, os, time, math, cmath, random, statistics, functools, threading, queue
from decimal import Decimal, ROUND_HALF_UP, InvalidOperation
from concurrent.futures import ProcessPoolExecutor

CENT = Decimal("0.01")


def money(x, places=CENT):
    """m(x): a Number as the decimal it is written as, rounded half up to the cent"""
    return Decimal(repr(x) if isinstance(x, float) else str(x)).quantize(places, ROUND_HALF_UP)


def split(amount, n):
    units = int(amount / CENT)
    base, extra = divmod(units, n)
    return [Decimal(base + (1 if i < extra else 0)) * CENT for i in range(n)]


def allocate(amount, ratios):
    units = int(amount / CENT)
    total = sum(ratios)
    raw = [units * r / total for r in ratios]
    shares = [int(x) for x in raw]
    left = units - sum(shares)
    for i in sorted(range(len(ratios)), key=lambda i: raw[i] - shares[i], reverse=True)[:left]:
        shares[i] += 1
    return [Decimal(s) * CENT for s in shares]


def fib(n):
    if n < 2:
        return n
    return fib(n - 1) + fib(n - 2)


class Account:
    def __init__(self):
        self.owner = ""
        self.balance = money(0)
        self.history = []

    def deposit(self, x):
        self.balance += x
        self.history.append(x)


def w_core():
    acc = fib(23)
    words = [f"w{i % 97}-{i % 13}" for i in range(20000)]
    srt = sorted(words)
    counts = {}
    for w in words:
        counts[w] = counts.get(w, 0) + 1
    acc += len(counts) + len(srt[0])
    seen = {}
    for i in range(5000):
        seen[(i % 50, i % 7)] = i
    acc += len(seen)
    text = ",".join(str(i) for i in range(5000))
    acc += len(text.split(",")) + len(text.upper()) + len(text.replace("1", "one"))
    squares = list(map(lambda x: x * x, range(20000)))
    acc += functools.reduce(lambda a, b: a + b, filter(lambda x: x % 3 == 0, squares), 0) % 1000003
    accts = []
    for i in range(300):
        a = Account()
        a.owner = f"o{i}"
        a.deposit(money(i))
        a.deposit(money(0.25))
        accts.append(a)
    acc += len(accts) + int(accts[299].balance / CENT)
    errors = 0
    for i in range(2000):
        try:
            xs = [1, 2]
            x = xs[i + 5]
        except IndexError:
            errors += 1
    acc += errors
    b = bytearray()
    for i in range(2000):
        b.append(i % 256)
    acc += len(b) + len(b.hex())
    return acc


def w_money():
    total = money(0)
    for i in range(20000):
        total += money(i * 0.01 + 0.005)
    shares = split(money(1000), 7)
    alloc = allocate(money(999.99), [1, 2, 3, 5, 8])
    text = ""
    for i in range(2000):
        text = "€" + f"{money(i * 1.37):,.2f}".translate(str.maketrans(",.", ".,"))
    yen = Decimal(0)
    rate = Decimal("151.234")
    for i in range(2000):
        yen += (money(i) * rate).quantize(Decimal(1), ROUND_HALF_UP)
    taxes = money(0)
    for i in range(10000):
        taxes += (money(19.99) * Decimal("0.0725")).quantize(CENT, ROUND_HALF_UP)
    return int(total / CENT) + len(shares) + int(alloc[4] / CENT) + len(text) + int(yen) + int(taxes / CENT)


def w_numbers():
    random.seed(12345)
    acc = 0
    for i in range(20000):
        acc += math.sin(i) * math.exp(-i / 20000) + math.hypot(i, 3) / 1e6 + math.lgamma(1 + i % 50) / 1e3
    acc += math.fsum(map(lambda i: 1 / (i + 1), range(10000)))
    acc += math.comb(60, 30) % 1000 + math.factorial(20) % 997 + math.gcd(123456, 7890)
    z = complex(0.5, 0.25)
    for i in range(5000):
        z = cmath.exp(cmath.log(z + 1) / 2)
    acc += z.real + cmath.phase(z)
    data = [random.gauss(10, 3) for i in range(5000)]
    acc += statistics.mean(data) + statistics.stdev(data) + statistics.median(data)
    acc += sum(statistics.quantiles(data, n=10))
    acc += statistics.correlation(data, [x * 2 + random.random() for x in data])
    acc += statistics.linear_regression(list(range(1000)), [3 * x + 1 for x in range(1000)]).slope
    nd = statistics.NormalDist(100, 15)
    acc += nd.cdf(130) + nd.inv_cdf(0.9) + (nd + 5).mean
    picked = random.sample(range(1000), 100)  # (in soak.lube's order: sample, shuffle, randint)
    deck = list(range(1000))
    random.shuffle(deck)
    acc += len(picked) + len(deck) + random.randint(1, 100)
    acc += sum(random.choices([1, 2, 3], weights=[1, 2, 3], k=100))
    return round(acc, 6)


def w_array():
    import numpy as np
    from safetensors.numpy import save_file, load_file
    np.random.seed(7)
    acc = 0
    a = np.random.rand(300, 300)
    b = np.random.rand(300, 300)
    acc += (a @ b).sum()
    f = a.astype(np.float32)
    acc += float((f @ f.T).sum())
    big = np.random.randn(200000)
    acc += np.sort(big)[1000] + np.argsort(big)[5] + np.median(big)
    acc += (big[big > 0] * 2).sum() + np.exp(big[:1000]).sum() + np.sqrt(np.abs(big)).mean()
    m2 = big.reshape(400, 500)
    acc += m2.sum(0).sum() + m2.T.mean(1).max() + m2[10:20, :].std() + m2[[1, 5, 9]].sum()
    acc += (m2 - m2.mean(0)).sum() + np.where(m2 > 1, m2, 0).sum()
    acc += len(np.unique(np.random.randint(0, 100, [5000])))
    s = np.linalg.solve(a + np.eye(300) * 10, np.ones(300))
    acc += s.sum() + np.linalg.det(np.eye(5) * 2) + np.linalg.norm(a)
    acc += np.linalg.eigh(a + a.T)[0].max()
    acc += np.linalg.svd(a[:50, :50])[1].sum()
    acc += np.concatenate([a[:5], b[:5]]).sum() + np.stack([a[0], b[0]]).sum()
    c = np.zeros(1000)
    for i in range(1000):
        c[i] = i
    c += 1
    c *= 0.5
    acc += c.sum()
    np.save(TMP + "/p.npy", a)
    acc += np.load(TMP + "/p.npy").sum()
    save_file({"a": a, "b": f}, TMP + "/p.safetensors")
    acc += load_file(TMP + "/p.safetensors")["a"].sum()
    return round(float(acc), 4)


def w_nn():
    import numpy as np, torch
    from torch import nn
    import torch.nn.functional as F
    from safetensors.torch import save_file, load_file
    torch.manual_seed(3)
    np.random.seed(3)
    X = torch.randn(256, 32)
    Y = torch.tensor(np.random.randint(0, 4, [256]))
    model = nn.Sequential(nn.Linear(32, 64), nn.ReLU(), nn.Linear(64, 64), nn.GELU(), nn.Linear(64, 4))
    opt = torch.optim.Adam(model.parameters(), lr=0.01)
    loss = 0
    for step in range(150):
        opt.zero_grad()
        l = F.cross_entropy(model(X), Y)
        l.backward()
        opt.step()
        loss = l.item()
    conv = nn.Sequential(nn.Conv2d(1, 4, 3, padding=1), nn.ReLU())
    sgd = torch.optim.SGD(conv.parameters(), lr=0.01, momentum=0.9)
    img = torch.randn(8, 1, 16, 16)
    for step in range(10):
        sgd.zero_grad()
        l = F.max_pool2d(conv(img), 2).mean()
        l.backward()
        sgd.step()
    x = torch.tensor([1.0, 2.0, 3.0], requires_grad=True)
    y = (torch.softmax(x * x, 0) * torch.tensor([1.0, 0.0, 0.0])).sum()
    y.backward()
    emb = nn.Embedding(50, 8)
    ln = nn.LayerNorm(8)
    z = ln(emb(torch.tensor([1, 2, 3]))).sum()
    save_file(model.state_dict(), TMP + "/p_model.safetensors")
    model.load_state_dict(load_file(TMP + "/p_model.safetensors"))
    with torch.no_grad():
        pred = model(X).argmax(1)
    return round(loss, 4) + round(float(x.grad.sum()), 6) + round(z.item(), 4) + int(pred.sum())


def w_learn():
    import numpy as np
    from sklearn.datasets import make_classification, make_blobs
    from sklearn.model_selection import train_test_split, cross_val_score
    from sklearn.ensemble import RandomForestClassifier
    from sklearn.preprocessing import StandardScaler
    from sklearn.linear_model import LogisticRegression, Ridge, Lasso
    from sklearn.neighbors import KNeighborsClassifier
    from sklearn.cluster import KMeans, DBSCAN
    from sklearn.decomposition import PCA
    from sklearn.tree import DecisionTreeRegressor
    from sklearn.naive_bayes import GaussianNB
    from sklearn.metrics import f1_score, classification_report, confusion_matrix, silhouette_score
    np.random.seed(11)
    X, y = make_classification(n_samples=2000, n_features=12, n_informative=6, n_classes=3, random_state=1)
    sp = train_test_split(X, y, test_size=0.25, random_state=0)
    acc = 0
    forest = RandomForestClassifier(n_estimators=40, random_state=0, n_jobs=-1).fit(sp[0], sp[2])
    acc += forest.score(sp[1], sp[3]) + forest.feature_importances_.sum()
    scaler = StandardScaler().fit(sp[0])
    logit = LogisticRegression(max_iter=200).fit(scaler.transform(sp[0]), sp[2])
    acc += logit.score(scaler.transform(sp[1]), sp[3])
    acc += KNeighborsClassifier(5).fit(sp[0], sp[2]).score(sp[1], sp[3])
    acc += round(KMeans(n_clusters=3, random_state=0).fit(X).inertia_, 2)
    acc += PCA(3).fit(X).explained_variance_ratio_.sum()
    acc += DecisionTreeRegressor(max_depth=6).fit(X, y).score(X, y)
    acc += Ridge(alpha=0.5).fit(X, y).score(X, y) + Lasso(alpha=0.01).fit(X, y).score(X, y)
    acc += GaussianNB().fit(X, y).score(X, y)
    acc += cross_val_score(GaussianNB(), X, y).sum()
    acc += f1_score(sp[3], forest.predict(sp[1]), average="macro")
    acc += len(classification_report(sp[3], forest.predict(sp[1])))
    acc += confusion_matrix(sp[3], logit.predict(scaler.transform(sp[1]))).sum()
    bX, by = make_blobs(n_samples=500, random_state=2)
    acc += DBSCAN(eps=0.8).fit_predict(bX).sum()
    acc += silhouette_score(bX, by)
    return round(float(acc), 4)


def w_llm():
    from llama_cpp import Llama
    model = Llama(model_path=MODEL, n_ctx=128, n_threads=THREADS, verbose=False)
    text = model.create_completion("Once upon a time", max_tokens=48, temperature=0)["choices"][0]["text"]
    ids = model.tokenize(text.encode())
    back = model.detokenize(ids)
    model.reset()
    model.eval(model.tokenize(b"The cat sat"))
    best = int(model.scores[model.n_tokens - 1].argmax())
    sampled = model.create_completion("The little dog", max_tokens=24, temperature=0.8, seed=42)["choices"][0]["text"]
    model.reset()
    acc = len(text) + len(ids) + len(back) + best + len(sampled)
    del model
    if MODEL2:
        m2 = Llama(model_path=MODEL2, n_ctx=256, n_threads=THREADS, verbose=False)
        acc += len(m2.create_completion("The capital of France is", max_tokens=12, temperature=0)["choices"][0]["text"])
        del m2
    return acc


def iso_work(n):
    import numpy as np, torch
    a = np.arange(n).reshape(10, n // 10)
    t = torch.tensor([1.0, 2.0, 3.0], requires_grad=True)
    l = (t * t).sum()
    l.backward()
    return float(a.sum(0).sum()) + float(money(n) * Decimal("1.5")) + float(t.grad.sum()) + statistics.mean([1, 2, n])


def chan_worker(jobs, results):
    while True:
        x = jobs.get()
        if x is None:
            return
        results.put(x * 2)


def w_isolates():
    acc = 0
    with ProcessPoolExecutor(8) as ex:
        acc += sum(ex.map(iso_work, [1000 + i * 10 for i in range(8)]))
    jobs, results = queue.Queue(), queue.Queue()
    workers = [threading.Thread(target=chan_worker, args=(jobs, results)) for _ in range(2)]
    for w in workers:
        w.start()
    for i in range(2000):
        jobs.put(money(i))
    for w in workers:
        jobs.put(None)
    for w in workers:
        w.join()
    acc += int(sum(results.get() for _ in range(2000)) / CENT)
    with ProcessPoolExecutor() as ex:
        acc += sum(ex.map(fib, [15 + i % 6 for i in range(40)]))
    return acc


def fails(f):
    try:
        f()
        return 0
    except Exception:
        return 1


def add_money(a, ca, b, cb):
    if ca != cb:
        raise ValueError(f"cannot add {a} {ca} and {b} {cb}: they are different currencies")
    return a + b


def w_errors():
    import numpy as np, torch
    from torch import nn
    import torch.nn.functional as F
    from sklearn.linear_model import Ridge
    from sklearn.cluster import KMeans
    from llama_cpp import Llama
    n = 0
    a = np.zeros((2, 3))
    net = nn.Linear(3, 2)
    for i in range(200):
        n += fails(lambda: a @ a)
        n += fails(lambda: a.reshape(4, 4))
        n += fails(lambda: a[5, 0])
        n += fails(lambda: np.array([[1, 2], [3]]))
        n += fails(lambda: np.linalg.inv(np.zeros((3, 3))))
        n += fails(lambda: Ridge().predict(a))
        n += fails(lambda: Ridge(alfa=1))
        n += fails(lambda: KMeans(3).fit(np.zeros((2, 2))))
        n += fails(lambda: net(torch.zeros(2, 5)))
        n += fails(lambda: F.cross_entropy(torch.zeros(2, 3), torch.tensor([5, 0])))
        n += fails(lambda: Llama(model_path=TMP + "/missing.gguf", verbose=False))
        n += fails(lambda: add_money(money(1), "USD", money(1), "EUR"))
        n += fails(lambda: Decimal("abc"))
        n += fails(lambda: statistics.variance([1]))
        n += fails(lambda: random.choice([]))
        n += fails(lambda: math.sqrt(-1))
        n += fails(lambda: complex("x"))
    return n


def w_tiny():
    import numpy as np, torch
    acc = 0
    small = np.array([1.0, 2.0, 3.0])
    for i in range(1000):
        acc += float((small + i)[0]) + float(small.sum()) + torch.tensor([i]).item() + int(money(i) / CENT)
        acc += statistics.mean([i, 1]) + math.floor(i / 3) + complex(i, 1).imag
    return acc


NAMES = ["core", "money", "numbers", "array", "nn", "learn", "llm", "isolates", "errors", "tiny"]
WORK = [w_core, w_money, w_numbers, w_array, w_nn, w_learn, w_llm, w_isolates, w_errors, w_tiny]

if __name__ == "__main__":
    MODEL, SECONDS, TMP = sys.argv[1], float(sys.argv[2]), sys.argv[3]
    MODEL2 = sys.argv[4] if len(sys.argv) > 4 else ""
    ONLY = sys.argv[5] if len(sys.argv) > 5 else ""
    THREADS = int(os.environ.get("LLAMA_THREADS", "0")) or None
    # the imports, once, before the clock (Lube's plugins load before its first line runs)
    import numpy, torch, sklearn.ensemble, llama_cpp, safetensors.numpy  # noqa
    print("H round elapsed " + " ".join(NAMES), flush=True)
    start = time.time()
    rnd = 0
    while rnd == 0 or time.time() - start < SECONDS:
        rnd += 1
        times, sums = [], []
        for name, f in zip(NAMES, WORK):
            if ONLY and ONLY != name:
                continue
            t0 = time.time()
            r = f()
            times.append(f"{(time.time() - t0) * 1000:.1f}")
            sums.append(str(r))
        print(f"R {rnd} {time.time() - start:.1f} " + " ".join(times) + " chk=" + "|".join(sums), flush=True)
