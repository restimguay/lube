"""llm.py: llama-cpp-python, llama.cpp underneath (llm.lube does the same with Lube's plugin llm): loading a GGUF
model, tokenizing, reading a prompt (the logits after it), generating greedily and with sampling. The models are
the extra arguments; the size is the model and the amount of text or tokens. Llama keeps its defaults (n_ctx 512,
its own thread count); repeat_penalty is 1, as Lube's is."""
from harness import EXTRA, bench, bench_chk, bench_nochk  # (first: see harness.py)
import os

import numpy as np
from llama_cpp import Llama


STORY = ("Once upon a time, there was a little girl named Lily. She loved to play outside in the sunshine. One day, she "
         "saw a big, red ball in the park. She ran to it and picked it up. It was soft and bouncy. Lily wanted to share "
         "the ball with her friend Tom, so she went to his house and knocked on the door. Tom was happy to see her and "
         "they played catch until the sun went down. ")


def text_of(n):
    t = ""
    while len(t) < n:
        t += STORY
    return t[:n]


def main():
    for path in EXTRA:
        label = os.path.basename(path).replace(".gguf", "")
        bench_chk("load", label, lambda: Llama(model_path=path, verbose=False), lambda m: m.n_vocab())
        model = Llama(model_path=path, verbose=False)
        n_vocab = model.n_vocab()
        for n in [1000, 100000]:
            text = text_of(n).encode()
            bench("tokenize", f"{label}/{n}ch", lambda: model.tokenize(text))
        ids = model.tokenize(text_of(100000).encode())
        bench("detokenize", f"{label}/100000ch", lambda: model.detokenize(ids))
        lengths = [16, 128, 384] if model._model.n_ctx_train() > 128 else [16, 100]
        for n in lengths:
            toks = ids[:n]

            def prompt_eval():
                model.reset()
                model.eval(toks)
                return int(np.argmax(np.ctypeslib.as_array(model._ctx.get_logits(), shape=(n_vocab,))))
            bench("prompt_eval", f"{label}/{n}tok", prompt_eval)

        def greedy():
            model.reset()
            return model.create_completion("Once upon a time", max_tokens=64, temperature=0,
                                           repeat_penalty=1.0)["choices"][0]["text"]
        bench("generate_greedy", f"{label}/64tok", greedy)

        def sampled():
            model.reset()
            return model.create_completion("Once upon a time", max_tokens=64, temperature=0.8, top_k=40, top_p=0.95,
                                           repeat_penalty=1.0, seed=42)["choices"][0]["text"]
        bench_nochk("generate_sampled", f"{label}/64tok", sampled)
        del model


if __name__ == "__main__":
    main()
