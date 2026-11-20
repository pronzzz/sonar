#!/usr/bin/env python3
"""Host tool for SonarFS disk images (format: docs/sonarfs.md).

  sonarfs.py mkimage IMAGE SIZE [--label L] [--put HOST:/path ...] [--mkdir /path ...]
        Create a GPT disk image with one SonarFS partition ("SONAR").
  sonarfs.py ls IMAGE [/path]          list a directory
  sonarfs.py get IMAGE /path [OUT]     copy a file out (stdout if no OUT)
  sonarfs.py put IMAGE HOSTFILE /path  copy a file in
  sonarfs.py fsck IMAGE                check consistency (bitmap, inodes, dirs)

SIZE accepts K/M/G suffixes. IMAGE may also be a bare SonarFS volume
(no partition table). This is an independent implementation of the format
used to cross-check the kernel's.
"""
import os
import struct
import sys
import time
import uuid
import zlib

BS = 4096
SECTOR = 512
ISZ = 128
IPB = BS // ISZ
DE = 64
NAME_MAX = 58
INLINE_EXT = 5
OVF_EXT = 255
ROOT = 1
FT_FILE, FT_DIR = 1, 2

SB_FMT = "<II8sIIQQQQQQQQQQQ32s16sQQ"  # 176 bytes
SB_JOURNAL = 1                          # flags: journal_start/journal_blocks are valid
JMAGIC = b"SNRJRNL1"
INODE_FMT = "<HHIQQQII"              # 40 bytes, then 5 extents, then u64
EXT_FMT = "<QII"

# GPT partition type for SonarFS volumes (an arbitrary, fixed GUID).
SONAR_TYPE = uuid.UUID("5a0e7a12-50fa-4f53-9d3e-536f6e617246")

assert struct.calcsize(SB_FMT) == 176
assert struct.calcsize(INODE_FMT) == 40


def parse_size(s):
    mult = {"K": 1 << 10, "M": 1 << 20, "G": 1 << 30}
    if s[-1].upper() in mult:
        return int(float(s[:-1]) * mult[s[-1].upper()])
    return int(s)


class Inode:
    def __init__(self, ino, raw=None):
        self.ino = ino
        if raw is None:
            raw = bytes(ISZ)
        (self.type, self.links, self.flags, self.size, self.ctime, self.mtime,
         self.nextents, self.ext_block) = struct.unpack_from(INODE_FMT, raw, 0)
        self.inline = [struct.unpack_from(EXT_FMT, raw, 40 + 16 * i)[:2] for i in range(INLINE_EXT)]
        self.extents = []


class Volume:
    def __init__(self, f, offset, size):
        self.f, self.off, self.size = f, offset, size
        self.writable = "+" in getattr(f, "mode", "") or "w" in getattr(f, "mode", "")

    # -- raw block access
    def rd(self, blk, n=1):
        self.f.seek(self.off + blk * BS)
        data = self.f.read(n * BS)
        return data + bytes(n * BS - len(data))

    def wr(self, blk, data):
        assert len(data) % BS == 0
        self.f.seek(self.off + blk * BS)
        self.f.write(data)

    # -- format
    def mkfs(self, label="SONAR"):
        total = min(self.size // BS, 0xFFFFFFFF)
        icount = max(256, min(65536, total // 16))
        icount = (icount + IPB - 1) // IPB * IPB
        bitmap_blocks = (total + BS * 8 - 1) // (BS * 8)
        inode_start = 1 + bitmap_blocks
        data_start = inode_start + icount // IPB
        if data_start + 8 > total:
            sys.exit("volume too small")
        now = int(time.time())
        used = data_start + 1
        self.sb = dict(version=1, flags=0, magic=b"SONARFS1", block_size=BS, inode_size=ISZ,
                       total_blocks=total, inode_count=icount, bitmap_start=1,
                       bitmap_blocks=bitmap_blocks, inode_start=inode_start,
                       inode_blocks=icount // IPB, data_start=data_start,
                       free_blocks=total - used, free_inodes=icount - 2, created=now, mtime=now,
                       label=label.encode()[:31], uuid=uuid.uuid4().bytes)
        # inode table
        zero = bytes(BS)
        for b in range(inode_start, data_start):
            self.wr(b, zero)
        # bitmap
        bm = bytearray(bitmap_blocks * BS)
        for b in range(used):
            bm[b >> 3] |= 1 << (b & 7)
        for b in range(total, bitmap_blocks * BS * 8):
            bm[b >> 3] |= 1 << (b & 7)
        self.bitmap = bm
        self.wr(1, bytes(bm))
        # root directory
        d = bytearray(BS)
        d[0:DE] = self.dirent(ROOT, FT_DIR, ".")
        d[DE:2 * DE] = self.dirent(ROOT, FT_DIR, "..")
        self.wr(data_start, bytes(d))
        root = Inode(ROOT)
        root.type, root.links, root.size = FT_DIR, 2, 2 * DE
        root.ctime = root.mtime = now
        root.extents = [(data_start, 1)]
        self.write_inode_raw(root)
        self.write_sb()

    # -- superblock / bitmap
    def load(self):
        raw = self.rd(0)
        v = struct.unpack_from(SB_FMT, raw, 0)
        keys = ["version", "flags", "magic", "block_size", "inode_size", "total_blocks",
                "inode_count", "bitmap_start", "bitmap_blocks", "inode_start", "inode_blocks",
                "data_start", "free_blocks", "free_inodes", "created", "mtime", "label", "uuid",
                "journal_start", "journal_blocks"]
        self.sb = dict(zip(keys, v))
        if self.sb["magic"] != b"SONARFS1" or self.sb["block_size"] != BS:
            sys.exit("not a SonarFS volume")
        if self.journal_pending():
            if not self.writable:
                print("note: the journal holds an update not yet written into place; "
                      "open the image writable (any write command) to finish it", file=sys.stderr)
            else:
                n = self.replay()
                print("finished an interrupted update from the journal (%d sectors)" % n,
                      file=sys.stderr)
                return self.load()
        self.bitmap = bytearray(self.rd(self.sb["bitmap_start"], self.sb["bitmap_blocks"]))

    # -- metadata journal (written by the kernel; see docs/sonarfs.md)
    def journal_header(self):
        if not (self.sb["flags"] & SB_JOURNAL) or not self.sb["journal_start"]:
            return None
        raw = self.rd(self.sb["journal_start"])
        magic, seq, count, crc_ = struct.unpack_from("<8sQII", raw, 0)
        if magic != JMAGIC or not 0 < count <= 504:
            return None
        lbas = struct.unpack_from("<%dQ" % count, raw, 24)
        self.f.seek(self.off + (self.sb["journal_start"] + 1) * BS)
        data = self.f.read(count * 512)
        if zlib.crc32(data, zlib.crc32(raw[24:24 + 8 * count])) & 0xFFFFFFFF != crc_:
            return None  # not committed: the update never happened
        return lbas, data

    def journal_pending(self):
        return self.journal_header() is not None

    def replay(self):
        lbas, data = self.journal_header()
        for i, lba in enumerate(lbas):
            self.f.seek(self.off + lba * 512)
            self.f.write(data[i * 512:(i + 1) * 512])
        self.wr(self.sb["journal_start"], bytes(BS))
        return len(lbas)

    def write_sb(self):
        s = self.sb
        raw = struct.pack(SB_FMT, s["version"], s["flags"], s["magic"], s["block_size"],
                          s["inode_size"], s["total_blocks"], s["inode_count"], s["bitmap_start"],
                          s["bitmap_blocks"], s["inode_start"], s["inode_blocks"], s["data_start"],
                          s["free_blocks"], s["free_inodes"], s["created"], int(time.time()),
                          s["label"].ljust(32, b"\0"), s["uuid"], s.get("journal_start", 0),
                          s.get("journal_blocks", 0))
        self.wr(0, raw + bytes(BS - len(raw)))
        self.wr(self.sb["bitmap_start"], bytes(self.bitmap))

    def used(self, b):
        return self.bitmap[b >> 3] >> (b & 7) & 1

    def mark(self, b, on):
        if self.used(b) == on:
            return
        if on:
            self.bitmap[b >> 3] |= 1 << (b & 7)
            self.sb["free_blocks"] -= 1
        else:
            self.bitmap[b >> 3] &= ~(1 << (b & 7)) & 0xFF
            self.sb["free_blocks"] += 1

    def alloc(self, want):
        """Allocate `want` blocks as few extents as possible (first fit)."""
        out, b, total = [], self.sb["data_start"], self.sb["total_blocks"]
        while want and b < total:
            if self.used(b):
                b += 1
                continue
            s = b
            while b < total and not self.used(b) and b - s < want:
                b += 1
            out.append((s, b - s))
            for x in range(s, b):
                self.mark(x, True)
            want -= b - s
        if want:
            sys.exit("no space left on volume")
        return out

    # -- inodes
    def inode_pos(self, ino):
        return self.sb["inode_start"] * BS + ino * ISZ

    def read_inode(self, ino):
        self.f.seek(self.off + self.inode_pos(ino))
        ip = Inode(ino, self.f.read(ISZ))
        ext = [e for e in ip.inline[:min(ip.nextents, INLINE_EXT)]]
        blk = ip.ext_block
        while len(ext) < ip.nextents and blk:
            raw = self.rd(blk)
            nxt, cnt = struct.unpack_from("<QI", raw, 0)
            for j in range(cnt):
                ext.append(struct.unpack_from(EXT_FMT, raw, 16 + 16 * j)[:2])
            blk = nxt
        ip.extents = ext
        return ip

    def write_inode_raw(self, ip, ovf_blocks=()):
        ip.nextents = len(ip.extents)
        raw = bytearray(struct.pack(INODE_FMT, ip.type, ip.links, ip.flags, ip.size, ip.ctime,
                                    ip.mtime, ip.nextents, ovf_blocks[0] if ovf_blocks else 0))
        for i in range(INLINE_EXT):
            s, l = ip.extents[i] if i < len(ip.extents) else (0, 0)
            raw += struct.pack(EXT_FMT, s, l, 0)
        raw += bytes(8)
        self.f.seek(self.off + self.inode_pos(ip.ino))
        self.f.write(raw)

    def save_inode(self, ip):
        # free the old overflow chain, write a new one if needed
        old = self.read_inode(ip.ino)
        blk = old.ext_block
        while blk:
            nxt = struct.unpack_from("<Q", self.rd(blk), 0)[0]
            self.mark(blk, False)
            blk = nxt
        rest = ip.extents[INLINE_EXT:]
        chunks = [rest[i:i + OVF_EXT] for i in range(0, len(rest), OVF_EXT)]
        blks = [self.alloc(1)[0][0] for _ in chunks]
        for i, ch in enumerate(chunks):
            raw = struct.pack("<QII", blks[i + 1] if i + 1 < len(blks) else 0, len(ch), 0)
            for s, l in ch:
                raw += struct.pack(EXT_FMT, s, l, 0)
            self.wr(blks[i], raw + bytes(BS - len(raw)))
        self.write_inode_raw(ip, blks)

    def new_inode(self, typ):
        for ino in range(2, self.sb["inode_count"]):
            self.f.seek(self.off + self.inode_pos(ino))
            if struct.unpack("<H", self.f.read(2))[0] == 0:
                ip = Inode(ino)
                ip.type, ip.links = typ, 1
                ip.ctime = ip.mtime = int(time.time())
                self.sb["free_inodes"] -= 1
                return ip
        sys.exit("no free inodes")

    # -- file data
    def read_data(self, ip):
        out = bytearray()
        for s, l in ip.extents:
            out += self.rd(s, l)
        return bytes(out[:ip.size])

    def write_data(self, ip, data):
        """Replace the contents of a (new or emptied) inode."""
        nblk = (len(data) + BS - 1) // BS
        ip.extents = self.alloc(nblk) if nblk else []
        pos = 0
        for s, l in ip.extents:
            chunk = data[pos:pos + l * BS]
            self.wr(s, chunk + bytes(l * BS - len(chunk)))
            pos += l * BS
        ip.size = len(data)
        ip.mtime = int(time.time())

    # -- directories
    @staticmethod
    def dirent(ino, typ, name):
        nb = name.encode()
        assert 0 < len(nb) <= NAME_MAX, name
        return struct.pack("<IBB", ino, typ, len(nb)) + nb.ljust(NAME_MAX, b"\0")

    def entries(self, dip):
        data = self.read_data(dip)
        out = []
        for i in range(0, len(data) - DE + 1, DE):
            ino, typ, nl = struct.unpack_from("<IBB", data, i)
            if ino:
                out.append((data[i + 6:i + 6 + nl].decode(errors="replace"), ino, typ, i))
        return out

    def lookup(self, path):
        ino = ROOT
        for part in [p for p in path.split("/") if p]:
            ip = self.read_inode(ino)
            if ip.type != FT_DIR:
                raise FileNotFoundError(path)
            for name, cino, _, _ in self.entries(ip):
                if name == part:
                    ino = cino
                    break
            else:
                raise FileNotFoundError(path)
        return self.read_inode(ino)

    def add_entry(self, dip, name, ino, typ):
        data = bytearray(self.read_data(dip))
        ent = self.dirent(ino, typ, name)
        for i in range(0, len(data), DE):
            if struct.unpack_from("<I", data, i)[0] == 0:
                data[i:i + DE] = ent
                break
        else:
            data += ent
        for s, l in dip.extents:
            for b in range(s, s + l):
                self.mark(b, False)
        self.write_data(dip, bytes(data))
        self.save_inode(dip)

    def split(self, path):
        parts = [p for p in path.split("/") if p]
        if not parts:
            sys.exit("bad path")
        return "/" + "/".join(parts[:-1]), parts[-1]

    def mkdir(self, path, parents=True):
        parts = [p for p in path.split("/") if p]
        cur = ""
        for p in parts:
            parent = cur or "/"
            cur = cur + "/" + p
            try:
                ip = self.lookup(cur)
                if ip.type != FT_DIR:
                    sys.exit(f"{cur} is not a directory")
                continue
            except FileNotFoundError:
                pass
            dip = self.lookup(parent)
            nd = self.new_inode(FT_DIR)
            nd.links = 2
            self.write_data(nd, self.dirent(nd.ino, FT_DIR, ".") + self.dirent(dip.ino, FT_DIR, ".."))
            self.save_inode(nd)
            dip.links += 1
            self.add_entry(dip, p, nd.ino, FT_DIR)

    def put(self, path, data):
        parent, name = self.split(path)
        self.mkdir(parent)
        try:
            old = self.lookup(path)
            sys.exit(f"{path} already exists") if old.type == FT_DIR else None
            for s, l in old.extents:
                for b in range(s, s + l):
                    self.mark(b, False)
            ip = old
            self.write_data(ip, data)
            self.save_inode(ip)
            return
        except FileNotFoundError:
            pass
        dip = self.lookup(parent)
        ip = self.new_inode(FT_FILE)
        self.write_data(ip, data)
        self.save_inode(ip)
        self.add_entry(dip, name, ip.ino, FT_FILE)

    # -- consistency check
    def fsck(self):
        errors = []
        owner = {}
        sb = self.sb
        for b in range(sb["data_start"]):
            owner[b] = "metadata"
        if sb["flags"] & SB_JOURNAL:
            for b in range(sb["journal_start"], sb["journal_start"] + sb["journal_blocks"]):
                owner[b] = "journal"
        seen_inodes = {}

        def claim(blk, who):
            if blk < sb["data_start"] or blk >= sb["total_blocks"]:
                errors.append(f"{who}: block {blk} outside data area")
            elif blk in owner:
                errors.append(f"{who}: block {blk} also used by {owner[blk]}")
            else:
                owner[blk] = who

        def visit(ino, path, parent):
            ip = self.read_inode(ino)
            if ip.type not in (FT_FILE, FT_DIR):
                errors.append(f"{path}: entry points to free inode {ino}")
                return
            seen_inodes[ino] = seen_inodes.get(ino, 0) + 1
            if seen_inodes[ino] > 1 and ip.type == FT_DIR:
                errors.append(f"{path}: directory linked twice")
                return
            nblk = sum(l for _, l in ip.extents)
            if (ip.size + BS - 1) // BS > nblk:
                errors.append(f"{path}: size {ip.size} exceeds {nblk} allocated blocks")
            if len(ip.extents) != ip.nextents:
                errors.append(f"{path}: extent count mismatch")
            for s, l in ip.extents:
                for b in range(s, s + l):
                    claim(b, path)
            blk = ip.ext_block
            while blk:
                claim(blk, path + " (extent block)")
                blk = struct.unpack_from("<Q", self.rd(blk), 0)[0]
            if ip.type == FT_DIR:
                ents = self.entries(ip)
                names = [e[0] for e in ents]
                if names.count(".") != 1 or names.count("..") != 1:
                    errors.append(f"{path}: missing or duplicate . / ..")
                subdirs = 0
                for name, cino, typ, _ in ents:
                    if name == ".":
                        if cino != ino:
                            errors.append(f"{path}: '.' points to {cino}")
                    elif name == "..":
                        if cino != parent:
                            errors.append(f"{path}: '..' points to {cino}, expected {parent}")
                    else:
                        cp = path.rstrip("/") + "/" + name
                        child = self.read_inode(cino)
                        if child.type != typ:
                            errors.append(f"{cp}: entry type {typ} != inode type {child.type}")
                        if child.type == FT_DIR:
                            subdirs += 1
                        visit(cino, cp, ino)
                if ip.links != 2 + subdirs:
                    errors.append(f"{path}: link count {ip.links}, expected {2 + subdirs}")

        visit(ROOT, "/", ROOT)
        free = 0
        for b in range(sb["total_blocks"]):
            u = self.used(b)
            if b in owner and not u:
                errors.append(f"block {b} ({owner[b]}) in use but marked free")
            if u and b not in owner:
                errors.append(f"block {b} marked used but unreferenced (leak)")
            free += not u
        if free != sb["free_blocks"]:
            errors.append(f"superblock free_blocks {sb['free_blocks']} != bitmap {free}")
        used_inodes = 0
        for ino in range(1, sb["inode_count"]):
            self.f.seek(self.off + self.inode_pos(ino))
            if struct.unpack("<H", self.f.read(2))[0]:
                used_inodes += 1
                if ino not in seen_inodes:
                    errors.append(f"inode {ino} allocated but not in any directory")
        if sb["inode_count"] - 1 - used_inodes != sb["free_inodes"]:
            errors.append(f"superblock free_inodes {sb['free_inodes']} != "
                          f"{sb['inode_count'] - 1 - used_inodes}")
        return errors, len(seen_inodes), sb["total_blocks"] - free


# ------------------------------------------------------------- GPT images
def crc(b):
    return zlib.crc32(b) & 0xFFFFFFFF


def make_gpt(f, size, name="SONAR"):
    nsec = size // SECTOR
    first, last = 2048, nsec - 34
    # protective MBR
    mbr = bytearray(SECTOR)
    mbr[446:462] = struct.pack("<BBBBBBBBII", 0, 0, 2, 0, 0xEE, 0xFF, 0xFF, 0xFF, 1,
                               min(nsec - 1, 0xFFFFFFFF))
    mbr[510:512] = b"\x55\xaa"
    ent = bytearray(128 * 128)
    pname = name.encode("utf-16-le")[:72]
    ent[0:128] = (SONAR_TYPE.bytes_le + uuid.uuid4().bytes_le + struct.pack("<QQQ", first, last, 0)
                  + pname.ljust(72, b"\0"))
    disk_guid = uuid.uuid4().bytes_le

    def header(my, alt, entries_lba):
        h = struct.pack("<8sIIIIQQQQ16sQIII", b"EFI PART", 0x10000, 92, 0, 0, my, alt, 34, nsec - 34,
                        disk_guid, entries_lba, 128, 128, crc(bytes(ent)))
        h = h[:16] + struct.pack("<I", crc(h)) + h[20:]
        return h + bytes(SECTOR - len(h))

    f.seek(0)
    f.write(bytes(mbr))
    f.write(header(1, nsec - 1, 2))
    f.write(bytes(ent))
    f.seek((nsec - 33) * SECTOR)
    f.write(bytes(ent))
    f.write(header(nsec - 1, 1, nsec - 33))
    return first * SECTOR, (last - first + 1) * SECTOR


def open_volume(path, mode="r+b"):
    f = open(path, mode)
    head = f.read(SECTOR * 2 + 128)
    if head[SECTOR:SECTOR + 8] == b"EFI PART":
        elba = struct.unpack_from("<Q", head, SECTOR + 72)[0]
        f.seek(elba * SECTOR)
        for _ in range(128):
            e = f.read(128)
            if e[:16] == SONAR_TYPE.bytes_le or (e[:16] != bytes(16) and _is_sonar(f, e)):
                s, l = struct.unpack_from("<QQ", e, 32)
                v = Volume(f, s * SECTOR, (l - s + 1) * SECTOR)
                v.load()
                return v
        sys.exit("no SonarFS partition in GPT")
    v = Volume(f, 0, os.path.getsize(path))
    v.load()
    return v


def _is_sonar(f, e):
    s = struct.unpack_from("<Q", e, 32)[0]
    pos = f.tell()
    f.seek(s * SECTOR + 8)
    ok = f.read(8) == b"SONARFS1"
    f.seek(pos)
    return ok


def main():
    a = sys.argv[1:]
    if not a:
        sys.exit(__doc__)
    cmd = a[0]
    if cmd == "mkimage":
        img, size = a[1], parse_size(a[2])
        label = a[a.index("--label") + 1] if "--label" in a else "SONAR"
        with open(img, "wb") as f:
            f.truncate(size)
        with open(img, "r+b") as f:
            off, vsize = make_gpt(f, size)
            v = Volume(f, off, vsize)
            v.mkfs(label)
            for d in ("/etc", "/home", "/models", "/tmp", "/var", "/var/log"):
                v.mkdir(d)
            i = 3
            while i < len(a):
                if a[i] == "--put":
                    host, dest = a[i + 1].rsplit(":", 1)
                    with open(host, "rb") as hf:
                        v.put(dest, hf.read())
                    i += 2
                elif a[i] == "--mkdir":
                    v.mkdir(a[i + 1])
                    i += 2
                else:
                    i += 1
            v.write_sb()
        print(f"{img}: {size >> 20} MiB GPT disk, SonarFS partition '{label}'")
    elif cmd == "ls":
        v = open_volume(a[1], "rb")
        ip = v.lookup(a[2] if len(a) > 2 else "/")
        for name, ino, typ, _ in sorted(v.entries(ip)):
            if name in (".", ".."):
                continue
            c = v.read_inode(ino)
            print(f"{'d' if typ == FT_DIR else '-'} {c.size:>12} {name}{'/' if typ == FT_DIR else ''}")
    elif cmd == "get":
        v = open_volume(a[1], "rb")
        data = v.read_data(v.lookup(a[2]))
        if len(a) > 3:
            open(a[3], "wb").write(data)
        else:
            sys.stdout.buffer.write(data)
    elif cmd == "put":
        v = open_volume(a[1])
        with open(a[2], "rb") as hf:
            v.put(a[3], hf.read())
        v.write_sb()
    elif cmd == "fsck":
        v = open_volume(a[1], "rb")
        errors, nino, used = v.fsck()
        for e in errors[:50]:
            print("  " + e)
        print(f"{'OK' if not errors else f'{len(errors)} PROBLEMS'}: {nino} inodes, {used} blocks in use")
        sys.exit(1 if errors else 0)
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
