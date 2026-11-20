#!/usr/bin/env python3
"""Reference BPE tokenizer for checking Sonar's tokenizer.

Builds a Hugging Face `tokenizers` BPE model from a GGUF file's own vocab and
merges, with the same pre-tokenizer regex llama.cpp uses for that model, then
compares its output with `build/aitest MODEL --tokfile` over a test corpus
(fixed cases plus random strings).

Usage: tokref.py MODEL.gguf [--fuzz N] [--seed S]
Requires: pip install tokenizers
"""
import os
import random
import struct
import subprocess
import sys
import tempfile

from tokenizers import Regex, Tokenizer, models, pre_tokenizers

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gguf_dump import Reader  # noqa: E402

# Pre-tokenizer patterns (llama.cpp llama-vocab.cpp / the models' tokenizer.json).
PATTERNS = {
    "qwen2": r"(?i:'s|'t|'re|'ve|'m|'ll|'d)|[^\r\n\p{L}\p{N}]?\p{L}+|\p{N}| ?[^\s\p{L}\p{N}]+[\r\n]*|\s*[\r\n]+|\s+(?!\S)|\s+",
    "llama3": r"(?i:'s|'t|'re|'ve|'m|'ll|'d)|[^\r\n\p{L}\p{N}]?\p{L}+|\p{N}{1,3}| ?[^\s\p{L}\p{N}]+[\r\n]*|\s*[\r\n]+|\s+(?!\S)|\s+",
}
PRE = {"qwen2": "qwen2", "deepseek-r1-qwen": "qwen2", "llama-bpe": "llama3"}

FIXED = [
    "Hello world",
    "Hello world! It's 2026, café 12345 naïve  spaces\n\tend",
    "I'M HERE, YOU'LL SEE, WE'D GO. it's they're we've I'd",
    "   leading spaces and trailing   ",
    "line1\nline2\r\nline3\n\n\nline4\r\r\n",
    "def f(x):\n    return x ** 2  # square\n\n\tpass",
    "numbers: 1 12 123 1234 12345 3.14159 -42 1e10 0x1F 1,000,000",
    "日本語のテキストです。中文文本测试。한국어 텍스트",
    "emoji 😀👍🏽 family 👨‍👩‍👧 flags 🇮🇳🇺🇸",
    "हिन्दी पाठ परीक्षण। संस्कृतम्",
    "Ελληνικά κείμενο, русский текст, עברית, العربية",
    "Ünïcödé àccents: ñ ç ø å ß œ æ",
    "<think>\nOkay, let me reason.\n</think>\n\nAnswer: 42",
    "tabs\t\tand\t spaces \t mixed",
    "!!!??? ... --- *** ### $$$ %%% ^^^ &&& ((( )))",
    "URL https://example.com/path?q=1&r=two#frag email a.b@c.de",
    "  \n  \n",
    " ",
    "\n",
    "a",
    "'s",
    "''''",
    "x'S y'Ll z'd",
    "₹100 €50 £20 ¥1000 ½ ¾ ² ³ Ⅻ ①",
    "zero​width nbsp sep",
    "mixed🙂text🙃with😉emoji",
    "    indented code\n        more\n",
    "CamelCaseIdentifier snake_case_name kebab-case-name",
    "The quick brown fox jumps over the lazy dog." * 3,
]

ALPHABETS = [
    "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ",
    "0123456789",
    " \t\n\r  \n\n",
    "'\".,;:!?()[]{}<>-_=+*/\\|@#$%^&~`",
    "éàüñçøåßœæÉÀÜ",
    "日本語中文한국어テキスト",
    "हिन्दीपाठ",  # includes combining marks (\p{M})
    "Ελληνικάрусский",
    "😀👍🏽🇮🇳‍",
    "²³½①Ⅻ₹€",
]


def load(path):
    with open(path, "rb") as f:
        r = Reader(f)
        if f.read(4) != b"GGUF":
            sys.exit("not a GGUF file")
        r.unpack("<I")
        r.unpack("<Q")
        n_kv = r.unpack("<Q")
        meta = {}
        for _ in range(n_kv):
            k = r.string()
            t = r.unpack("<I")
            meta[k] = r.value(t)
    return meta


def build(meta):
    tokens = meta["tokenizer.ggml.tokens"]
    merges = meta["tokenizer.ggml.merges"]
    pre = PRE.get(meta.get("tokenizer.ggml.pre", ""), None)
    if pre is None:
        sys.exit(f"unsupported pre-tokenizer {meta.get('tokenizer.ggml.pre')!r}")
    vocab = {}
    for i, t in enumerate(tokens):
        vocab.setdefault(t, i)
    pairs = [tuple(m.split(" ", 1)) for m in merges]
    bpe = models.BPE(vocab=vocab, merges=pairs, ignore_merges=(pre == "llama3"))
    tok = Tokenizer(bpe)
    tok.pre_tokenizer = pre_tokenizers.Sequence([
        pre_tokenizers.Split(Regex(PATTERNS[pre]), behavior="isolated", invert=False),
        pre_tokenizers.ByteLevel(add_prefix_space=False, use_regex=False),
    ])
    return tok, pre


def fuzz(n, seed):
    rng = random.Random(seed)
    out = []
    for _ in range(n):
        k = rng.randint(1, 4)
        alpha = "".join(rng.sample(ALPHABETS, k))
        out.append("".join(rng.choice(alpha) for _ in range(rng.randint(1, 60))))
    return out


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    path = sys.argv[1]
    n_fuzz = int(sys.argv[sys.argv.index("--fuzz") + 1]) if "--fuzz" in sys.argv else 2000
    seed = int(sys.argv[sys.argv.index("--seed") + 1]) if "--seed" in sys.argv else 1
    meta = load(path)
    tok, pre = build(meta)
    cases = FIXED + fuzz(n_fuzz, seed)
    here = os.path.dirname(os.path.abspath(__file__))
    aitest = os.path.join(here, "..", "build", "aitest")
    with tempfile.NamedTemporaryFile("wb", delete=False) as tf:
        tf.write(b"\0".join(c.encode("utf-8") for c in cases))
        name = tf.name
    try:
        res = subprocess.run([aitest, path, "--tokfile", name], capture_output=True, check=True)
    finally:
        os.unlink(name)
    lines = res.stdout.decode().split("\n")
    bad = 0
    for i, c in enumerate(cases):
        want = tok.encode(c).ids
        got = [int(x) for x in lines[i].split()] if lines[i].strip() else []
        if want != got:
            bad += 1
            if bad <= 10:
                print(f"MISMATCH case {i}: {c!r}\n  ref:   {want}\n  sonar: {got}")
    print(f"pre-tokenizer {pre}: {len(cases) - bad}/{len(cases)} cases identical")
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
