# Makes the tiny GGUF models in this folder for tests/llm.lube, and prints what a numpy reference
# implementation of each gives (logits and greedy tokens), which tests/llm.lube checks the plugin against.
#
#   python3 tests/models/make_models.py          (needs numpy; writes tests/models/*.gguf)
#
# The weights are random (numpy's generator, fixed seeds), in every format llm reads. The reference follows
# llama.cpp's way, as llm does: quantized weights multiply the vector quantized to int8 in blocks of 32.
import struct, sys, os
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))

# ---------- GGUF files ----------

T_F32, T_F16, T_Q4_0, T_Q4_1, T_Q5_0, T_Q5_1, T_Q8_0, T_Q4_K, T_Q5_K, T_Q6_K, T_BF16 = 0, 1, 2, 3, 6, 7, 8, 12, 13, 14, 30
U32, I32, F32, BOOL, STR, ARR = 4, 5, 6, 7, 8, 9

def gstr(s):
    b = s.encode() if isinstance(s, str) else s
    return struct.pack("<Q", len(b)) + b

def gvalue(t, v):
    if t == U32: return struct.pack("<I", v)
    if t == I32: return struct.pack("<i", v)
    if t == F32: return struct.pack("<f", v)
    if t == BOOL: return struct.pack("<B", 1 if v else 0)
    if t == STR: return gstr(v)
    et, items = v
    return struct.pack("<IQ", et, len(items)) + b"".join(gvalue(et, x) for x in items)

def write_gguf(path, kvs, tensors):
    """kvs: [(key, type, value)]; tensors: [(name, ggml type, ne (row length first), bytes)]"""
    out = b"GGUF" + struct.pack("<IQQ", 3, len(tensors), len(kvs))
    for k, t, v in kvs: out += gstr(k) + struct.pack("<I", t) + gvalue(t, v)
    offset, data = 0, b""
    for name, t, ne, raw in tensors:
        out += gstr(name) + struct.pack("<I", len(ne)) + b"".join(struct.pack("<Q", n) for n in ne)
        out += struct.pack("<IQ", t, offset)
        pad = (-len(raw)) % 32
        data += raw + b"\0" * pad
        offset += len(raw) + pad
    out += b"\0" * ((-len(out)) % 32)
    with open(path, "wb") as f: f.write(out + data)
    return len(out + data)

# ---------- the number formats: quantize (bytes) and the weights the bytes stand for (float64) ----------

def f16(x): return np.float16(x)

def quantize(t, w):
    """w: rows x cols float. Returns (bytes, the dequantized weights as llm reads them)."""
    w = np.asarray(w, np.float32)
    if t == T_F32: return w.tobytes(), w.astype(np.float64)
    if t == T_F16: h = w.astype(np.float16); return h.tobytes(), h.astype(np.float64)
    if t == T_BF16:
        b = (w.view(np.uint32) >> 16).astype(np.uint16)  # (truncated: any bfloat16 will do)
        return b.tobytes(), (b.astype(np.uint32) << 16).view(np.float32).astype(np.float64)
    rows = []
    deq = []
    for row in w:
        rb, rd = b"", []
        if t in (T_Q4_K, T_Q5_K, T_Q6_K):
            for blk in row.reshape(-1, 256):
                b, d = quant_k(t, blk); rb += b; rd.append(d)
        else:
            for blk in row.reshape(-1, 32):
                b, d = quant_32(t, blk); rb += b; rd.append(d)
        rows.append(rb); deq.append(np.concatenate(rd))
    return b"".join(rows), np.array(deq)

def quant_32(t, x):
    x = x.astype(np.float64)
    if t == T_Q8_0:
        d = f16(np.abs(x).max() / 127); q = np.clip(np.round(x / float(d)) if d else np.zeros(32), -127, 127).astype(np.int8)
        return d.tobytes() + q.tobytes(), float(d) * q
    if t in (T_Q4_0, T_Q5_0):
        top = 8 if t == T_Q4_0 else 16
        mx = x[np.argmax(np.abs(x))]; d = f16(mx / -top)
        q = np.clip(np.floor(x / float(d) + top + 0.5), 0, 2 * top - 1).astype(np.int64) if d else np.full(32, top)
        deq = float(d) * (q - top)
        if t == T_Q4_0: return d.tobytes() + (q[:16] | (q[16:] << 4)).astype(np.uint8).tobytes(), deq
        qh = sum(int(q[j] >> 4) << j for j in range(32))
        return d.tobytes() + struct.pack("<I", qh) + ((q[:16] & 15) | ((q[16:] & 15) << 4)).astype(np.uint8).tobytes(), deq
    # Q4_1, Q5_1: a scale and a minimum
    top = 15 if t == T_Q4_1 else 31
    lo, hi = x.min(), x.max(); d = f16((hi - lo) / top); m = f16(lo)
    q = np.clip(np.round((x - float(m)) / float(d)), 0, top).astype(np.int64) if d else np.zeros(32, np.int64)
    deq = float(d) * q + float(m)
    if t == T_Q4_1: return d.tobytes() + m.tobytes() + (q[:16] | (q[16:] << 4)).astype(np.uint8).tobytes(), deq
    qh = sum(int(q[j] >> 4) << j for j in range(32))
    return d.tobytes() + m.tobytes() + struct.pack("<I", qh) + ((q[:16] & 15) | ((q[16:] & 15) << 4)).astype(np.uint8).tobytes(), deq

def pack_scales_k4(sc, mn):
    """8 six-bit scales and mins into 12 bytes, as ggml's get_scale_min_k4 reads them"""
    b = [0] * 12
    for j in range(8):
        if j < 4: b[j] |= sc[j]; b[j + 4] |= mn[j]
        else:
            b[j + 4] |= (sc[j] & 15) | ((mn[j] & 15) << 4)
            b[j - 4] |= (sc[j] >> 4) << 6
            b[j] |= (mn[j] >> 4) << 6
    return bytes(b)

def quant_k(t, x):
    x = x.astype(np.float64)
    if t == T_Q6_K:
        sub = x.reshape(16, 16)
        scale = np.array([s[np.argmax(np.abs(s))] / -32 for s in sub])
        d = f16(np.abs(scale).max() / 127)
        sc = np.clip(np.round(scale / float(d)), -128, 127).astype(np.int64) if d else np.zeros(16, np.int64)
        q = np.zeros(256, np.int64); deq = np.zeros(256)
        for j in range(16):
            ds = float(d) * sc[j]
            qq = np.clip(np.round(sub[j] / ds), -32, 31).astype(np.int64) if ds else np.zeros(16, np.int64)
            q[16 * j:16 * j + 16] = qq + 32; deq[16 * j:16 * j + 16] = float(np.float32(float(d) * sc[j])) * qq
        # (llm reads element 128h + 32t + l from ql/qh as ggml lays them out)
        ql = np.zeros(128, np.int64); qh = np.zeros(64, np.int64)
        for h in range(2):
            for l in range(32):
                q1, q2, q3, q4 = (q[128 * h + l + 32 * k] for k in range(4))
                ql[64 * h + l] = (q1 & 15) | ((q3 & 15) << 4)
                ql[64 * h + l + 32] = (q2 & 15) | ((q4 & 15) << 4)
                qh[32 * h + l] = (q1 >> 4) | ((q2 >> 4) << 2) | ((q3 >> 4) << 4) | ((q4 >> 4) << 6)
        # the scales: sub-block j of 16 numbers is element 16j..; ggml's order is the same (is = l/16 + 2k per half)
        order = []
        for h in range(2):
            for k in range(4):
                for half in range(2): order.append(8 * h + 2 * k + half)
        scales = np.zeros(16, np.int64)
        for j in range(16): scales[order[j]] = sc[j]
        return ql.astype(np.uint8).tobytes() + qh.astype(np.uint8).tobytes() + scales.astype(np.int8).tobytes() + d.tobytes(), deq
    top = 15 if t == T_Q4_K else 31
    sub = x.reshape(8, 32)
    mins = np.maximum(0, -sub.min(axis=1)); scales = (sub.max(axis=1) + mins) / top
    d = f16(scales.max() / 63); dmin = f16(mins.max() / 63)
    sc = np.clip(np.round(scales / float(d)), 0, 63).astype(np.int64) if d else np.zeros(8, np.int64)
    mn = np.clip(np.round(mins / float(dmin)), 0, 63).astype(np.int64) if dmin else np.zeros(8, np.int64)
    q = np.zeros(256, np.int64); deq = np.zeros(256)
    for j in range(8):
        a = float(np.float32(float(d) * sc[j])); b = float(np.float32(float(dmin) * mn[j]))
        qq = np.clip(np.round((sub[j] + b) / a), 0, top).astype(np.int64) if a else np.zeros(32, np.int64)
        q[32 * j:32 * j + 32] = qq; deq[32 * j:32 * j + 32] = a * qq - b
    qs = np.zeros(128, np.int64); qhb = np.zeros(32, np.int64)
    for j in range(4):
        lo, hi = q[64 * j:64 * j + 32], q[64 * j + 32:64 * j + 64]
        qs[32 * j:32 * j + 32] = (lo & 15) | ((hi & 15) << 4)
        qhb |= ((lo >> 4) << (2 * j)) | ((hi >> 4) << (2 * j + 1))
    head = d.tobytes() + dmin.tobytes() + pack_scales_k4(sc, mn)
    if t == T_Q4_K: return head + qs.astype(np.uint8).tobytes(), deq
    return head + qhb.astype(np.uint8).tobytes() + qs.astype(np.uint8).tobytes(), deq

# ---------- tokenizers ----------

def spm_vocab():
    """SentencePiece's: tokens, scores, types (1 normal, 2 unknown, 3 control, 4 user-defined, 6 byte)"""
    toks = [("<unk>", 0, 2), ("<s>", 0, 3), ("</s>", 0, 3), ("<0x0A>", 0, 6), ("<0xC3>", 0, 6), ("<0xA9>", 0, 6),
            ("<|user|>", 0, 4), ("<|end|>", 0, 3)]
    pieces = ["▁", "a", "c", "d", "e", "g", "h", "i", "l", "m", "n", "o", "p", "r", "s", "t", "u", "w", "y", ".", ",", "!",
              "▁t", "he", "▁the", "▁a", "at", "▁c", "▁cat", "▁s", "▁sat", "on", "▁on",
              "▁m", "▁mat", "▁d", "og", "▁dog", "▁w", "as", "▁was", "pp", "ha", "▁ha",
              "▁happ", "▁happy", "in", "▁in"]
    for i, p in enumerate(pieces): toks.append((p, -100.0 + 2 * i if len(p) > 1 else -1000.0 - i, 1))
    return toks

BPE_CHARS = "Ġ Ċ a b c d e g h i l m n o p r s t u w y 0 1 2 3 4 5 . , ! ' ? H".split()
BPE_MERGES = ["Ġ t", "h e", "Ġt he", "Ġ a", "a t", "Ġ c", "Ġc at", "Ġ s", "Ġs at", "o n", "Ġ on", "i n", "Ġ in", "1 2",
              "' s", "Ġ m", "Ġm at", "Ġ d", "o g", "Ġd og", "Ġ h", "Ġh at", "e l", "el l", "Ġh ell", "Ġhell o"]

def bpe_vocab():
    toks = [(c, 0, 1) for c in BPE_CHARS]
    for m in BPE_MERGES: toks.append((m.replace(" ", ""), 0, 1))
    toks.append(("Ġcats", 0, 1))  # (a word no merges make: llama 3's rules take it whole)
    toks += [("<|endoftext|>", 0, 3), ("<|im_start|>", 0, 3), ("<|im_end|>", 0, 3), ("<tool>", 0, 4)]
    return toks

# ---------- models ----------

class Spec:
    def __init__(self, **kw): self.__dict__.update(kw)

def make_model(s):
    rng = np.random.RandomState(s.seed)
    E, L, H, KV, FF = s.embd, s.layers, s.heads, s.kv_heads, s.ff
    hd = s.head_dim if getattr(s, "head_dim", None) else E // H
    toks = spm_vocab() if s.vocab == "spm" else bpe_vocab()
    V = len(toks)
    a = s.arch
    kvs = [("general.architecture", STR, a), ("general.name", STR, s.name)]
    if getattr(s, "file_type", None) is not None: kvs.append(("general.file_type", U32, s.file_type))
    kvs += [(a + ".context_length", U32, 64), (a + ".embedding_length", U32, E), (a + ".block_count", U32, L),
            (a + ".feed_forward_length", U32, FF), (a + ".attention.head_count", U32, H),
            (a + ".attention.head_count_kv", U32, KV), (a + ".attention.layer_norm_rms_epsilon", F32, 1e-5),
            (a + ".rope.freq_base", F32, 10000.0)]
    if getattr(s, "head_dim", None): kvs.append((a + ".attention.key_length", U32, hd))
    if getattr(s, "rope_linear", None): kvs += [(a + ".rope.scaling.type", STR, "linear"), (a + ".rope.scaling.factor", F32, s.rope_linear)]
    if s.vocab == "spm":
        kvs += [("tokenizer.ggml.model", STR, "llama"), ("tokenizer.ggml.tokens", ARR, (STR, [t[0] for t in toks])),
                ("tokenizer.ggml.scores", ARR, (F32, [t[1] for t in toks])), ("tokenizer.ggml.token_type", ARR, (I32, [t[2] for t in toks])),
                ("tokenizer.ggml.bos_token_id", U32, 1), ("tokenizer.ggml.eos_token_id", U32, 2), ("tokenizer.ggml.unknown_token_id", U32, 0)]
    else:
        kvs += [("tokenizer.ggml.model", STR, "gpt2"), ("tokenizer.ggml.pre", STR, s.pre),
                ("tokenizer.ggml.tokens", ARR, (STR, [t[0] for t in toks])), ("tokenizer.ggml.token_type", ARR, (I32, [t[2] for t in toks])),
                ("tokenizer.ggml.merges", ARR, (STR, BPE_MERGES)),
                ("tokenizer.ggml.bos_token_id", U32, V - 4), ("tokenizer.ggml.eos_token_id", U32, V - 4),
                ("tokenizer.ggml.add_bos_token", BOOL, getattr(s, "add_bos", False))]
    W = {}
    tensors = []
    def add(name, t, w):
        raw, deq = quantize(t, w)
        W[name] = (t, deq)
        tensors.append((name, t, [w.shape[1]] if w.shape[0] == 1 else [w.shape[1], w.shape[0]], raw))
    def mat(rows, cols, scale=1.0): return rng.normal(0, scale / np.sqrt(cols), (rows, cols))
    def norm(n): return 1 + 0.2 * rng.normal(0, 1, (1, n))
    ty = s.types
    add("token_embd.weight", ty["embd"], rng.normal(0, 2 / np.sqrt(E), (V, E)))
    if "output" in ty: add("output.weight", ty["output"], mat(V, E, 2.0))
    add("output_norm.weight", T_F32, norm(E))
    if getattr(s, "rope_freqs", False): add("rope_freqs.weight", T_F32, (1 + rng.uniform(0, 3, (1, hd // 2))))
    for l in range(L):
        p = "blk.%d." % l
        lt = ty["layers"][l % len(ty["layers"])]
        add(p + "attn_norm.weight", T_F32, norm(E))
        add(p + "ffn_norm.weight", T_F32, norm(E))
        if s.fused:
            add(p + "attn_qkv.weight", lt["q"], mat(H * hd + 2 * KV * hd, E))
        else:
            add(p + "attn_q.weight", lt["q"], mat(H * hd, E))
            add(p + "attn_k.weight", lt["k"], mat(KV * hd, E))
            add(p + "attn_v.weight", lt["v"], mat(KV * hd, E))
        if getattr(s, "bias", False):
            add(p + "attn_q.bias", T_F32, 0.1 * rng.normal(0, 1, (1, H * hd)))
            add(p + "attn_k.bias", T_F32, 0.1 * rng.normal(0, 1, (1, KV * hd)))
            add(p + "attn_v.bias", T_F32, 0.1 * rng.normal(0, 1, (1, KV * hd)))
        if getattr(s, "qk_norm", False):
            add(p + "attn_q_norm.weight", T_F32, norm(hd))
            add(p + "attn_k_norm.weight", T_F32, norm(hd))
        add(p + "attn_output.weight", lt["o"], mat(E, H * hd))
        if s.fused:
            add(p + "ffn_up.weight", lt["up"], mat(2 * FF, E))
        else:
            add(p + "ffn_gate.weight", lt["gate"], mat(FF, E))
            add(p + "ffn_up.weight", lt["up"], mat(FF, E))
        add(p + "ffn_down.weight", lt["down"], mat(E, FF))
    size = write_gguf(os.path.join(HERE, s.file), kvs, tensors)
    return Reference(s, W, V, hd), size

# ---------- the reference forward pass ----------

def quant_a8(x):
    """x quantized as llm does (int8 in blocks of 32, scale amax / 127, rounded to even), as floats again"""
    x = np.asarray(x, np.float32).reshape(-1, 32)
    amax = np.abs(x).max(axis=1)
    d = (amax / np.float32(127)).astype(np.float32)
    inv = np.where(d != 0, np.float32(1) / np.where(d != 0, d, 1), 0).astype(np.float32)
    q = np.rint((x * inv[:, None]).astype(np.float32))
    return (q * d[:, None].astype(np.float64)).reshape(-1)

class Reference:
    def __init__(self, s, W, V, hd):
        self.s, self.W, self.V, self.hd = s, W, V, hd
        self.reset()
    def reset(self):
        self.kc = [[] for _ in range(self.s.layers)]; self.vc = [[] for _ in range(self.s.layers)]; self.pos = 0
    def mm(self, name, x):
        t, w = self.W[name]
        xx = np.asarray(x, np.float32).astype(np.float64)
        return w @ (xx if t in (T_F32, T_F16, T_BF16) else quant_a8(xx))
    def vec(self, name): return self.W[name][1].reshape(-1)
    def rms(self, x, name):
        x = np.asarray(x, np.float32).astype(np.float64)
        return x / np.sqrt(np.mean(x * x) + 1e-5) * self.vec(name)
    def rope(self, x, pos):
        s, hd = self.s, self.hd
        half = hd // 2
        inv = 10000.0 ** (-2.0 * np.arange(half) / hd) / getattr(s, "rope_linear", 1)
        if getattr(s, "rope_freqs", False): inv = inv / self.vec("rope_freqs.weight")
        c, sn = np.cos(pos * inv), np.sin(pos * inv)
        x = x.copy()
        neox = s.arch != "llama" and s.arch != "mistral"
        ia = np.arange(half) if neox else 2 * np.arange(half)
        ib = ia + half if neox else ia + 1
        a, b = x[:, ia].copy(), x[:, ib].copy()
        x[:, ia] = a * c - b * sn; x[:, ib] = a * sn + b * c
        return x
    def step(self, tok):
        s, hd = self.s, self.hd
        E, H, KV, FF = s.embd, s.heads, s.kv_heads, s.ff
        x = self.W["token_embd.weight"][1][tok].copy()
        if s.arch == "gemma": x = x * np.float32(np.sqrt(E))
        for l in range(s.layers):
            p = "blk.%d." % l
            h = self.rms(x, p + "attn_norm.weight")
            if s.fused:
                qkv = self.mm(p + "attn_qkv.weight", h); q, k, v = qkv[:H * hd], qkv[H * hd:H * hd + KV * hd], qkv[H * hd + KV * hd:]
            else:
                q, k, v = self.mm(p + "attn_q.weight", h), self.mm(p + "attn_k.weight", h), self.mm(p + "attn_v.weight", h)
            if getattr(s, "bias", False): q = q + self.vec(p + "attn_q.bias"); k = k + self.vec(p + "attn_k.bias"); v = v + self.vec(p + "attn_v.bias")
            q, k, v = q.reshape(H, hd), k.reshape(KV, hd), v.reshape(KV, hd)
            if getattr(s, "qk_norm", False):
                q = np.stack([self.rms(r, p + "attn_q_norm.weight") for r in q]); k = np.stack([self.rms(r, p + "attn_k_norm.weight") for r in k])
            q, k = self.rope(q, self.pos), self.rope(k, self.pos)
            self.kc[l].append(k); self.vc[l].append(v)
            K, Vv = np.stack(self.kc[l]), np.stack(self.vc[l])
            out = np.zeros((H, hd))
            for i in range(H):
                g = i // (H // KV)
                sc = K[:, g] @ q[i] / np.sqrt(hd); sc = np.exp(sc - sc.max()); sc /= sc.sum()
                out[i] = sc @ Vv[:, g]
            x = x + self.mm(p + "attn_output.weight", out.reshape(-1))
            h = self.rms(x, p + "ffn_norm.weight")
            if s.fused: gu = self.mm(p + "ffn_up.weight", h); g, u = gu[:FF], gu[FF:]
            else: g, u = self.mm(p + "ffn_gate.weight", h), self.mm(p + "ffn_up.weight", h)
            act = 0.5 * g * (1 + np.tanh(0.7978845608 * (g + 0.044715 * g ** 3))) if s.arch == "gemma" else g / (1 + np.exp(-g))
            x = x + self.mm(p + "ffn_down.weight", act * u)
        self.pos += 1
        out = "output.weight" if "output.weight" in self.W else "token_embd.weight"
        return self.mm(out, self.rms(x, "output_norm.weight"))
    def run(self, tokens, n_greedy):
        self.reset()
        for t in tokens: lg = self.step(t)
        first = lg.copy()
        greedy, margin = [], 1e9
        for _ in range(n_greedy):
            srt = np.sort(lg); margin = min(margin, srt[-1] - srt[-2])
            t = int(lg.argmax()); greedy.append(t); lg = self.step(t)
        return first, greedy, margin

LAYERS_MIXED = [dict(q=T_F32, k=T_F16, v=T_BF16, o=T_Q4_0, gate=T_Q4_1, up=T_Q5_0, down=T_Q5_1),
                dict(q=T_Q8_0, k=T_Q4_0, v=T_Q5_1, o=T_F16, gate=T_BF16, up=T_Q8_0, down=T_F32)]
Q8 = [dict(q=T_Q8_0, k=T_Q8_0, v=T_Q8_0, o=T_Q8_0, gate=T_Q8_0, up=T_Q8_0, down=T_Q8_0)]
Q4 = [dict(q=T_Q4_0, k=T_Q4_0, v=T_Q4_0, o=T_Q4_0, gate=T_Q4_0, up=T_Q4_0, down=T_Q4_0)]
KQ = [dict(q=T_Q5_K, k=T_Q6_K, v=T_Q4_K, o=T_Q8_0, gate=T_Q4_K, up=T_Q6_K, down=T_Q5_0)]

SPECS = [
    Spec(file="llama-mixed.gguf", name="tiny llama", arch="llama", seed=1, embd=32, layers=2, heads=4, kv_heads=2, ff=64,
         vocab="spm", fused=False, types=dict(embd=T_Q8_0, output=T_F16, layers=LAYERS_MIXED), file_type=7,
         prompt=[1, 30, 36, 38, 40]),
    Spec(file="qwen2-bpe.gguf", name="tiny qwen2", arch="qwen2", seed=2, embd=32, layers=1, heads=2, kv_heads=1, ff=64,
         vocab="bpe", pre="qwen2", fused=False, bias=True, types=dict(embd=T_Q8_0, layers=Q8), prompt=[60, 41, 50, 42]),
    Spec(file="phi3-fused.gguf", name="tiny phi3", arch="phi3", seed=3, embd=32, layers=1, heads=4, kv_heads=4, ff=32,
         vocab="spm", fused=True, types=dict(embd=T_Q4_0, output=T_Q4_0, layers=Q4), prompt=[1, 33, 37]),
    Spec(file="kquants.gguf", name="tiny k-quants", arch="llama", seed=4, embd=256, layers=1, heads=2, kv_heads=1, ff=32,
         head_dim=16, vocab="bpe", pre="llama-bpe", add_bos=True, fused=False, rope_freqs=True, rope_linear=2.0,
         types=dict(embd=T_Q4_K, layers=KQ), prompt=[60, 41, 50, 42, 45]),
    Spec(file="gemma.gguf", name="tiny gemma", arch="gemma", seed=5, embd=32, layers=1, heads=2, kv_heads=1, ff=32,
         vocab="spm", fused=False, types=dict(embd=T_F16, layers=Q8), prompt=[1, 30, 36]),
    Spec(file="qwen3.gguf", name="tiny qwen3", arch="qwen3", seed=6, embd=32, layers=1, heads=2, kv_heads=2, ff=32,
         vocab="bpe", pre="smollm", fused=False, qk_norm=True, types=dict(embd=T_Q4_0, layers=Q4), prompt=[41, 50]),
    Spec(file="mistral-gpt2.gguf", name="tiny mistral", arch="mistral", seed=7, embd=32, layers=1, heads=2, kv_heads=1,
         ff=32, vocab="bpe", pre="gpt2", fused=False, types=dict(embd=T_Q8_0, layers=Q4), prompt=[41, 50, 42]),
]

if __name__ == "__main__":
    total = 0
    print("// made by tests/models/make_models.py: the reference's logits (top 5) and greedy tokens")
    for s in SPECS:
        ref, size = make_model(s)
        total += size
        first, greedy, margin = ref.run(s.prompt, 8)
        top = [int(i) for i in np.argsort(-first)[:5]]
        print('  ["%s", %s, %s, %s, %s],  // margin %.3f' % (s.file, s.prompt, top, [round(float(first[i]), 4) for i in top], greedy, margin))
        names = {"llama-mixed.gguf": ["blk.0.attn_q.weight", "blk.0.attn_k.weight", "blk.0.attn_v.weight", "blk.0.attn_output.weight",
                                      "blk.0.ffn_gate.weight", "blk.0.ffn_up.weight", "blk.0.ffn_down.weight", "blk.1.attn_q.weight"],
                 "kquants.gguf": ["blk.0.attn_q.weight", "blk.0.attn_k.weight", "blk.0.attn_v.weight"]}
        for name in names.get(s.file, []):  # each format's first numbers, as llm reads them
            print('  // %s %s' % (name, [round(float(v), 6) for v in ref.W[name][1][0][:4]]))
    print("// total %d bytes" % total)
