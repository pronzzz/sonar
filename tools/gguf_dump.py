#!/usr/bin/env python3
"""Inspect a GGUF model file: print metadata keys and tensor layout.

Usage: gguf_dump.py MODEL.gguf [--tensors] [--key NAME]
Pure standard library; reads only the header (no weights).
"""
import struct
import sys

GGUF_TYPES = {
    0: ("u8", "<B"), 1: ("i8", "<b"), 2: ("u16", "<H"), 3: ("i16", "<h"),
    4: ("u32", "<I"), 5: ("i32", "<i"), 6: ("f32", "<f"), 7: ("bool", "<?"),
    8: ("str", None), 9: ("arr", None), 10: ("u64", "<Q"), 11: ("i64", "<q"),
    12: ("f64", "<d"),
}

GGML_TYPES = {
    0: "F32", 1: "F16", 2: "Q4_0", 3: "Q4_1", 6: "Q5_0", 7: "Q5_1", 8: "Q8_0",
    9: "Q8_1", 10: "Q2_K", 11: "Q3_K", 12: "Q4_K", 13: "Q5_K", 14: "Q6_K",
    15: "Q8_K", 16: "IQ2_XXS", 17: "IQ2_XS", 18: "IQ3_XXS", 19: "IQ1_S",
    20: "IQ4_NL", 21: "IQ3_S", 22: "IQ2_S", 23: "IQ4_XS", 24: "I8", 25: "I16",
    26: "I32", 27: "I64", 28: "F64", 29: "IQ1_M", 30: "BF16",
}


class Reader:
    def __init__(self, f):
        self.f = f

    def unpack(self, fmt):
        size = struct.calcsize(fmt)
        return struct.unpack(fmt, self.f.read(size))[0]

    def string(self):
        n = self.unpack("<Q")
        return self.f.read(n).decode("utf-8", errors="replace")

    def value(self, t):
        name, fmt = GGUF_TYPES[t]
        if name == "str":
            return self.string()
        if name == "arr":
            et = self.unpack("<I")
            n = self.unpack("<Q")
            return [self.value(et) for _ in range(n)]
        return self.unpack(fmt)


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)
    path = sys.argv[1]
    show_tensors = "--tensors" in sys.argv
    key = None
    if "--key" in sys.argv:
        key = sys.argv[sys.argv.index("--key") + 1]
    with open(path, "rb") as f:
        r = Reader(f)
        magic = f.read(4)
        if magic != b"GGUF":
            sys.exit("not a GGUF file")
        version = r.unpack("<I")
        n_tensors = r.unpack("<Q")
        n_kv = r.unpack("<Q")
        print(f"GGUF v{version}: {n_tensors} tensors, {n_kv} metadata keys")
        meta = {}
        for _ in range(n_kv):
            k = r.string()
            t = r.unpack("<I")
            meta[k] = r.value(t)
        for k, v in meta.items():
            if key and k != key:
                continue
            if isinstance(v, list):
                if key:
                    print(f"{k} = {v!r}")
                else:
                    print(f"{k} = [{len(v)} items] first: {v[:8]!r}")
            elif isinstance(v, str) and len(v) > 200 and not key:
                print(f"{k} = <str {len(v)} chars> {v[:120]!r}...")
            else:
                print(f"{k} = {v!r}")
        if show_tensors:
            types = {}
            for _ in range(n_tensors):
                name = r.string()
                nd = r.unpack("<I")
                dims = [r.unpack("<Q") for _ in range(nd)]
                tt = r.unpack("<I")
                off = r.unpack("<Q")
                tn = GGML_TYPES.get(tt, str(tt))
                types[tn] = types.get(tn, 0) + 1
                if not name.startswith("blk.") or name.startswith("blk.0."):
                    print(f"  {name:32s} {tn:6s} {dims} @ {off}")
            print("tensor types:", types)


if __name__ == "__main__":
    main()
