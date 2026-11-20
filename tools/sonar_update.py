#!/usr/bin/env python3
"""Sign and check Sonar update manifests (docs/updates.md).

    sonar_update.py keygen [KEYFILE]          make a signing key (refuses to overwrite)
    sonar_update.py pubkey [KEYFILE]          print the public key (hex)
    sonar_update.py header [KEYFILE] > OUT.h  C header with the public key, for the kernel
    sonar_update.py manifest IMAGE VERSION [--key KEYFILE] [--file NAME] > sonar-update.txt
    sonar_update.py verify MANIFEST [IMAGE] (--pub HEX | --key KEYFILE | --header FILE.h)
    sonar_update.py bundle MANIFEST IMAGE OUT  /boot/kernel bundle (what `update install` writes)

The key is 32 random bytes (an Ed25519 seed), stored as 64 hex digits.
KEYFILE defaults to $SONAR_UPDATE_KEY_FILE, else ~/.config/sonar/update_signing_key.
If the environment variable SONAR_UPDATE_SIGNING_KEY holds the hex seed
(GitHub Actions secret), it is used instead of a file.

Ed25519 here is the RFC 8032 reference algorithm in plain Python (no
dependencies), so it runs the same on a laptop and in CI.
"""
import hashlib
import os
import sys

# ------------------------------------------------------------- Ed25519
P = 2 ** 255 - 19
L = 2 ** 252 + 27742317777372353535851937790883648493
D = -121665 * pow(121666, P - 2, P) % P
SQRTM1 = pow(2, (P - 1) // 4, P)


def _add(p, q):
    x1, y1, z1, t1 = p
    x2, y2, z2, t2 = q
    a = (y1 - x1) * (y2 - x2) % P
    b = (y1 + x1) * (y2 + x2) % P
    c = 2 * t1 * t2 * D % P
    d = 2 * z1 * z2 % P
    e, f, g, h = b - a, d - c, d + c, b + a
    return (e * f % P, g * h % P, f * g % P, e * h % P)


def _mul(s, p):
    q = (0, 1, 1, 0)
    while s:
        if s & 1:
            q = _add(q, p)
        p = _add(p, p)
        s >>= 1
    return q


def _recover_x(y, sign):
    if y >= P:
        return None
    x2 = (y * y - 1) * pow(D * y * y + 1, P - 2, P)
    if x2 == 0:
        return None if sign else 0
    x = pow(x2, (P + 3) // 8, P)
    if (x * x - x2) % P:
        x = x * SQRTM1 % P
    if (x * x - x2) % P:
        return None
    if (x & 1) != sign:
        x = P - x
    return x


_GY = 4 * pow(5, P - 2, P) % P
_GX = _recover_x(_GY, 0)
G = (_GX, _GY, 1, _GX * _GY % P)


def _encode(p):
    zi = pow(p[2], P - 2, P)
    x, y = p[0] * zi % P, p[1] * zi % P
    return int.to_bytes(y | ((x & 1) << 255), 32, "little")


def _decode(b):
    y = int.from_bytes(b, "little")
    sign = y >> 255
    y &= (1 << 255) - 1
    x = _recover_x(y, sign)
    if x is None:
        return None
    return (x, y, 1, x * y % P)


def _h(m):
    return int.from_bytes(hashlib.sha512(m).digest(), "little")


def _expand(seed):
    h = hashlib.sha512(seed).digest()
    a = int.from_bytes(h[:32], "little")
    a &= (1 << 254) - 8
    a |= 1 << 254
    return a, h[32:]


def public_key(seed):
    a, _ = _expand(seed)
    return _encode(_mul(a, G))


def sign(seed, msg):
    a, prefix = _expand(seed)
    pub = _encode(_mul(a, G))
    r = _h(prefix + msg) % L
    R = _encode(_mul(r, G))
    s = (r + _h(R + pub + msg) * a) % L
    return R + int.to_bytes(s, 32, "little")


def verify(pub, msg, sig):
    if len(sig) != 64 or len(pub) != 32:
        return False
    A = _decode(pub)
    R = _decode(sig[:32])
    s = int.from_bytes(sig[32:], "little")
    if A is None or R is None or s >= L:
        return False
    k = _h(sig[:32] + pub + msg) % L
    return _encode(_mul(s, G)) == _encode(_add(R, _mul(k, A)))


# ------------------------------------------------------------- keys
def default_key_file():
    return os.environ.get("SONAR_UPDATE_KEY_FILE") or os.path.expanduser(
        "~/.config/sonar/update_signing_key")


def load_seed(path=None):
    env = os.environ.get("SONAR_UPDATE_SIGNING_KEY", "").strip()
    if env and not path:
        text = env
    else:
        path = path or default_key_file()
        try:
            with open(path) as f:
                text = f.read().strip()
        except FileNotFoundError:
            sys.exit("no signing key: set SONAR_UPDATE_SIGNING_KEY or run `%s keygen`"
                     " (looked for %s)" % (sys.argv[0], path))
    try:
        seed = bytes.fromhex(text)
    except ValueError:
        seed = b""
    if len(seed) != 32:
        sys.exit("the signing key must be 64 hex digits")
    return seed


# ---------------------------------------------------------- manifests
MANIFEST_MAGIC = "sonar-update 1"


def make_manifest(image, version, seed, name=None):
    data = open(image, "rb").read()
    pub = public_key(seed)
    body = (
        "%s\n"
        "version %s\n"
        "arch aarch64\n"
        "file %s\n"
        "size %d\n"
        "sha256 %s\n"
        "key %s\n" % (MANIFEST_MAGIC, version, name or os.path.basename(image), len(data),
                      hashlib.sha256(data).hexdigest(), pub.hex()[:16])
    ).encode()
    return body + b"ed25519 " + sign(seed, body).hex().encode() + b"\n"


def parse_manifest(raw):
    """Return (signed_body, signature, fields) or raise ValueError."""
    marker = raw.rfind(b"\ned25519 ")
    if marker < 0:
        raise ValueError("no ed25519 line")
    body = raw[:marker + 1]
    sig_hex = raw[marker + 9:].strip()
    sig = bytes.fromhex(sig_hex.decode())
    lines = body.decode().splitlines()
    if not lines or lines[0] != MANIFEST_MAGIC:
        raise ValueError("not a Sonar update manifest")
    fields = {}
    for line in lines[1:]:
        k, _, v = line.partition(" ")
        fields[k] = v
    return body, sig, fields


def pub_from_header(path):
    text = open(path).read()
    hexes = text.split("{", 1)[1].split("}", 1)[0]
    return bytes(int(x, 16) for x in hexes.replace(",", " ").split())


BUNDLE_MAGIC = b"SONARUPD"
BUNDLE_HEADER = 4096


def make_bundle(manifest, image):
    """The file `update install` writes to /boot/kernel on SonarFS:
    8-byte magic, u32 manifest length, u32 0, manifest, zeros to 4096, image."""
    if len(manifest) > BUNDLE_HEADER - 16:
        raise ValueError("manifest too long")
    head = BUNDLE_MAGIC + len(manifest).to_bytes(4, "little") + bytes(4) + manifest
    return head + bytes(BUNDLE_HEADER - len(head)) + image


def main(argv):
    if len(argv) < 2:
        sys.exit(__doc__)
    cmd = argv[1]
    if cmd == "keygen":
        path = argv[2] if len(argv) > 2 else default_key_file()
        if os.path.exists(path):
            sys.exit("%s exists; not overwriting it" % path)
        os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
        fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        seed = os.urandom(32)
        os.write(fd, (seed.hex() + "\n").encode())
        os.close(fd)
        print("wrote %s; public key %s" % (path, public_key(seed).hex()))
    elif cmd == "pubkey":
        print(public_key(load_seed(argv[2] if len(argv) > 2 else None)).hex())
    elif cmd == "header":
        pub = public_key(load_seed(argv[2] if len(argv) > 2 else None))
        print("/* Ed25519 public key for signed updates. Generated by")
        print(" * tools/sonar_update.py header; see docs/updates.md. */")
        print("#define UPDATE_PUBKEY_HEX \"%s\"" % pub.hex())
        print("static const u8 update_pubkey[32] = {")
        for i in range(0, 32, 8):
            print("    " + ", ".join("0x%02x" % b for b in pub[i:i + 8]) + ",")
        print("};")
    elif cmd == "manifest":
        args = argv[2:]
        key = name = None
        if "--key" in args:
            i = args.index("--key")
            key = args[i + 1]
            del args[i:i + 2]
        if "--file" in args:
            i = args.index("--file")
            name = args[i + 1]
            del args[i:i + 2]
        if len(args) != 2:
            sys.exit("usage: manifest IMAGE VERSION [--key KEYFILE] [--file NAME]")
        sys.stdout.buffer.write(make_manifest(args[0], args[1], load_seed(key), name))
    elif cmd == "verify":
        args = argv[2:]
        pub = None
        for opt in ("--pub", "--key", "--header"):
            if opt in args:
                i = args.index(opt)
                v = args[i + 1]
                del args[i:i + 2]
                pub = (bytes.fromhex(v) if opt == "--pub" else
                       public_key(load_seed(v)) if opt == "--key" else pub_from_header(v))
        if pub is None or not args:
            sys.exit("usage: verify MANIFEST [IMAGE] (--pub HEX | --key FILE | --header FILE.h)")
        body, sig, fields = parse_manifest(open(args[0], "rb").read())
        if not verify(pub, body, sig):
            sys.exit("BAD SIGNATURE")
        print("signature OK: version %s, %s bytes" % (fields.get("version"), fields.get("size")))
        if len(args) > 1:
            data = open(args[1], "rb").read()
            if len(data) != int(fields["size"]) or hashlib.sha256(data).hexdigest() != fields["sha256"]:
                sys.exit("IMAGE DOES NOT MATCH the manifest")
            print("image OK")
    elif cmd == "bundle":
        if len(argv) != 5:
            sys.exit("usage: bundle MANIFEST IMAGE OUT")
        with open(argv[4], "wb") as f:
            f.write(make_bundle(open(argv[2], "rb").read(), open(argv[3], "rb").read()))
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main(sys.argv)
