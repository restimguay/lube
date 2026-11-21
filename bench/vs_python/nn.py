"""nn.py: PyTorch (nn.lube does the same with Lube's plugin nn): tensor operations, Linear, Conv2d, the activations,
LayerNorm, Embedding, cross_entropy, backward, Adam and SGD steps, training steps, saving and loading. Every weight
and input is drawn from numpy's RandomState (as Lube's array draws them) and given to both sides, so the results
can be compared. Forward passes run under torch.no_grad(); the size is the shape of the input. PyTorch keeps its
own thread count (torch.get_num_threads())."""
from harness import TMP, bench, bench_chk  # (first: see harness.py)
import math
import os

import numpy as np
import torch
import torch.nn.functional as F
from safetensors.torch import load_file, save_file
from torch import nn


rnd = np.random


def f32(a):
    return torch.from_numpy(np.ascontiguousarray(a, dtype=np.float32))


def uniform(shape, fan_in):
    return f32((rnd.rand(*shape) * 2 - 1) / math.sqrt(fan_in))


def mlp(sizes, seed):
    layers, state = [], {}
    for i in range(len(sizes) - 1):
        if i > 0:
            layers.append(nn.ReLU())
        layers.append(nn.Linear(sizes[i], sizes[i + 1]))
    model = nn.Sequential(*layers)
    rnd.seed(seed)
    for i in range(len(sizes) - 1):
        state[f"{2 * i}.weight"] = uniform([sizes[i + 1], sizes[i]], sizes[i])
        state[f"{2 * i}.bias"] = uniform([sizes[i + 1]], sizes[i])
    model.load_state_dict(state)
    return model


def first_param(model):
    return next(model.parameters()).detach().numpy().copy()


def no_grad(f):
    def run():
        with torch.no_grad():
            return f()
    return run


def main():
    # ---------- tensor operations ----------
    for shape in [[64, 1024], [1024, 1024]]:
        size = f"{shape[0]}x{shape[1]}"
        rnd.seed(30)
        a = f32(rnd.randn(*shape))
        b = f32(rnd.randn(*shape))
        w = f32(rnd.randn(shape[1], 256) / 32)
        bench("tensor_add", size, lambda: a + b)
        bench("tensor_mul", size, lambda: a * b)
        bench("tensor_matmul_x256", size, lambda: a @ w)
        bench("tensor_sum", size, lambda: a.sum())
        bench("tensor_mean_dim1", size, lambda: a.mean(1))
        bench("tensor_exp", size, lambda: a.exp())
        bench("tensor_transpose_matmul", size, lambda: a.T @ b)

        # ---------- activations (modules) ----------
        acts = [("ReLU", nn.ReLU()), ("GELU", nn.GELU()), ("Tanh", nn.Tanh()), ("Sigmoid", nn.Sigmoid()),
                ("Softmax", nn.Softmax(1)), ("LogSoftmax", nn.LogSoftmax(1)), ("SiLU", nn.SiLU())]
        for name, m in acts:
            bench(name, size, lambda m=m: m(a))

    # ---------- layers ----------
    for batch in [64, 1024]:
        lin = nn.Linear(784, 256)
        ln = nn.LayerNorm(512)
        emb = nn.Embedding(10000, 128)
        rnd.seed(31)
        lin.load_state_dict({"weight": uniform([256, 784], 784), "bias": uniform([256], 784)})
        X = f32(rnd.rand(batch, 784))
        H = f32(rnd.randn(batch, 512))
        emb.load_state_dict({"weight": f32(rnd.randn(10000, 128))})
        ids = torch.from_numpy(rnd.randint(0, 10000, (batch // 2, 32)))
        bench("Linear_784_256", f"{batch}x784", no_grad(lambda: lin(X)))
        bench("LayerNorm_512", f"{batch}x512", no_grad(lambda: ln(H)))
        bench("Embedding_10000x128", f"{batch // 2}x32", no_grad(lambda: emb(ids)))

    for batch in [8, 64]:
        conv = nn.Conv2d(3, 16, 3, padding=1)
        pool = nn.MaxPool2d(2)
        rnd.seed(32)
        conv.load_state_dict({"weight": uniform([16, 3, 3, 3], 27), "bias": uniform([16], 27)})
        img = f32(rnd.randn(batch, 3, 32, 32))
        fmap = f32(rnd.randn(batch, 16, 32, 32))
        bench("Conv2d_3_16_k3", f"{batch}x3x32x32", no_grad(lambda: conv(img)))
        bench("MaxPool2d_2", f"{batch}x16x32x32", no_grad(lambda: pool(fmap)))

    for shape in [[64, 10], [4096, 1000]]:
        rnd.seed(33)
        logits = f32(rnd.randn(*shape))
        labels = torch.from_numpy(rnd.randint(0, shape[1], shape[0]))
        bench("cross_entropy", f"{shape[0]}x{shape[1]}", lambda: F.cross_entropy(logits, labels))

    # ---------- training: backward, optimizer steps, whole steps ----------
    for sizes, batch in [([784, 256, 10], 64), ([784, 1024, 1024, 10], 512)]:
        size = "-".join(str(s) for s in sizes) + f"/b{batch}"
        model = mlp(sizes, 340)
        rnd.seed(34)
        X = f32(rnd.rand(batch, 784))
        Y = torch.from_numpy(rnd.randint(0, 10, batch))
        adam = torch.optim.Adam(model.parameters(), lr=0.001)

        def forward_backward():
            adam.zero_grad()
            loss = F.cross_entropy(model(X), Y)
            loss.backward()
            return loss.item()
        bench("forward_backward", size, forward_backward)

        def adam_step():
            adam.step()
            return 0
        bench_chk("adam_step", size, adam_step, lambda r: first_param(model))

        model_s = mlp(sizes, 341)
        sgd = torch.optim.SGD(model_s.parameters(), lr=0.01, momentum=0.9)
        F.cross_entropy(model_s(X), Y).backward()

        def sgd_step():
            sgd.step()
            return 0
        bench_chk("sgd_step", size, sgd_step, lambda r: first_param(model_s))

        model2 = mlp(sizes, 342)
        opt2 = torch.optim.Adam(model2.parameters(), lr=0.001)

        def train_step():
            opt2.zero_grad()
            loss = F.cross_entropy(model2(X), Y)
            loss.backward()
            opt2.step()
            return loss.item()
        bench("train_step_adam", size, train_step)

        fresh = mlp(sizes, 343)
        path = f"{TMP}/nn_{batch}_py.safetensors"
        save_file(fresh.state_dict(), path)  # (first, so that each case can run alone)

        def save():
            save_file(fresh.state_dict(), path)
            return 0
        bench("save_state_dict", size, save)
        bench("load_state_dict_file", size, lambda: load_file(path))
        os.remove(path)

    for batch in [32, 128]:
        cnn = nn.Sequential(nn.Conv2d(1, 8, 3, padding=1), nn.ReLU(), nn.MaxPool2d(2), nn.Flatten(),
                            nn.Linear(8 * 14 * 14, 10))
        rnd.seed(35)
        cnn.load_state_dict({"0.weight": uniform([8, 1, 3, 3], 9), "0.bias": uniform([8], 9),
                             "4.weight": uniform([10, 8 * 14 * 14], 8 * 14 * 14),
                             "4.bias": uniform([10], 8 * 14 * 14)})
        imgs = f32(rnd.randn(batch, 1, 28, 28))
        labels = torch.from_numpy(rnd.randint(0, 10, batch))
        opt = torch.optim.SGD(cnn.parameters(), lr=0.01, momentum=0.9)

        def cnn_step():
            opt.zero_grad()
            loss = F.cross_entropy(cnn(imgs), labels)
            loss.backward()
            opt.step()
            return loss.item()
        bench("cnn_train_step_sgd", f"{batch}x1x28x28", cnn_step)


if __name__ == "__main__":
    main()
