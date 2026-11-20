#!/usr/bin/env python3
"""Build a bootable UEFI disk image: GPT with one EFI System Partition (FAT32).

    mkesp.py OUT.img [--size MiB] [--system SYSTEM.img] DEST=SOURCE ...

DEST is a path on the partition, e.g. EFI/BOOT/BOOTAA64.EFI=build/Image.
Files larger than FAT32's 4 GiB limit are split into DEST, DEST.1, DEST.2,
... (2 GiB each); Sonar's UEFI loader reads the parts back as one file.

--system adds a second partition holding the SonarFS volume of SYSTEM.img
(a GPT image from `sonarfs.py mkimage`, or a bare volume). Sonar mounts it
from the disk itself, so changes are kept; without it the system image goes
on the EFI partition as sonar/system.img, which the loader copies into RAM
(changes are lost at power-off).

The image can be written to a USB stick (dd, or a tool like balenaEtcher)
or given to QEMU as a disk. Formats follow the UEFI specification (GPT,
chapter 5) and Microsoft's FAT32 specification (fatgen103), including
long file names.
"""
import argparse
import os
import struct
import sys
import uuid
import zlib

SECTOR = 512
ESP_TYPE = uuid.UUID("c12a7328-f81f-11d2-ba4b-00a0c93ec93b")
SONAR_TYPE = uuid.UUID("5a0e7a12-50fa-4f53-9d3e-536f6e617246")  # tools/sonarfs.py
PART_START = 2048                 # 1 MiB alignment
FAT32_MAX = (1 << 32) - 1
SPLIT = 2 << 30                   # part size for split files


# ---------------------------------------------------------------- FAT32
class Node:
    def __init__(self, name, is_dir, source=None, size=0, offset=0):
        self.name, self.is_dir, self.source, self.size, self.offset = name, is_dir, source, size, offset
        self.children = []
        self.cluster = 0


def short_ok(name):
    if name.upper() != name and name.lower() != name:
        pass
    base, _, ext = name.partition(".")
    valid = set("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-~!#$%&'(){}^@`")
    return ("." not in ext and 1 <= len(base) <= 8 and len(ext) <= 3 and
            all(c in valid for c in (base + ext).upper()) and name == name.upper())


def short_name(name, taken):
    if short_ok(name):
        base, _, ext = name.partition(".")
        return (base.ljust(8) + ext.ljust(3)).encode()
    stem, dot, ext = name.rpartition(".")
    if not dot:
        stem, ext = name, ""
    clean = lambda s: "".join(c for c in s.upper() if c.isalnum() or c in "_-")
    stem, ext = clean(stem) or "FILE", clean(ext)[:3]
    for i in range(1, 1000000):
        tail = "~%d" % i
        cand = (stem[:8 - len(tail)] + tail).ljust(8) + ext.ljust(3)
        if cand not in taken:
            return cand.encode()
    raise SystemExit("too many similar names")


def lfn_checksum(sn):
    s = 0
    for c in sn:
        s = (((s & 1) << 7) + (s >> 1) + c) & 0xFF
    return s


def dir_entries(node, parent_cluster):
    """Raw 32-byte entries for a directory's contents."""
    out = []
    if parent_cluster is not None:  # not root: . and ..
        out.append(entry(b".          ", 0x10, node.cluster, 0))
        out.append(entry(b"..         ", 0x10, parent_cluster, 0))
    taken = set()
    for ch in node.children:
        sn = short_name(ch.name, taken)
        taken.add(sn.decode())
        if not short_ok(ch.name):
            units = [ord(c) for c in ch.name] + [0]
            while len(units) % 13:
                units.append(0xFFFF)
            n = len(units) // 13
            cs = lfn_checksum(sn)
            for seq in range(n, 0, -1):
                u = units[(seq - 1) * 13: seq * 13]
                e = bytearray(32)
                e[0] = seq | (0x40 if seq == n else 0)
                e[1:11] = struct.pack("<5H", *u[0:5])
                e[11], e[12], e[13] = 0x0F, 0, cs
                e[14:26] = struct.pack("<6H", *u[5:11])
                e[28:32] = struct.pack("<2H", *u[11:13])
                out.append(bytes(e))
        out.append(entry(sn, 0x10 if ch.is_dir else 0x20, ch.cluster, 0 if ch.is_dir else ch.size))
    return b"".join(out)


def entry(sn, attr, cluster, size):
    date = ((2026 - 1980) << 9) | (1 << 5) | 1
    return struct.pack("<11sBBBHHHHHHHI", sn, attr, 0, 0, 0, date, date, cluster >> 16, 0, date,
                       cluster & 0xFFFF, size)


def sonar_volume(path):
    """(offset, length) of the SonarFS volume in an image: its GPT partition, or the whole file."""
    with open(path, "rb") as f:
        head = f.read(SECTOR * 2)
        if head[SECTOR:SECTOR + 8] == b"EFI PART":
            elba, = struct.unpack_from("<Q", head, SECTOR + 72)
            f.seek(elba * SECTOR)
            for _ in range(128):
                e = f.read(128)
                if e[:16] == SONAR_TYPE.bytes_le:
                    first, last = struct.unpack_from("<QQ", e, 32)
                    return first * SECTOR, (last - first + 1) * SECTOR
            raise SystemExit("%s: no SonarFS partition" % path)
        f.seek(8)
        if f.read(8) != b"SONARFS1":
            raise SystemExit("%s: not a SonarFS image" % path)
        return 0, os.path.getsize(path)


def build(out, size_mib, files, system=None):
    root = Node("", True)
    for dest, src in files:
        parts = [p for p in dest.replace("\\", "/").split("/") if p]
        node = root
        for d in parts[:-1]:
            nxt = next((c for c in node.children if c.name.upper() == d.upper() and c.is_dir), None)
            if not nxt:
                nxt = Node(d, True)
                node.children.append(nxt)
            node = nxt
        total = os.path.getsize(src)
        if total <= FAT32_MAX:
            node.children.append(Node(parts[-1], False, src, total, 0))
        else:
            for i, off in enumerate(range(0, total, SPLIT)):
                name = parts[-1] if i == 0 else "%s.%d" % (parts[-1], i)
                node.children.append(Node(name, False, src, min(SPLIT, total - off), off))

    data_bytes = sum(n.size for n in walk(root) if not n.is_dir)
    if size_mib is None:
        size_mib = max(64, (data_bytes * 105 // 100 >> 20) + 48)
    total_sectors = size_mib * (1 << 20) // SECTOR
    part_sectors = total_sectors - PART_START - 34
    if system:  # second partition, 1 MiB aligned, after the EFI partition
        sys_off, sys_len = sonar_volume(system)
        sys_start = -(-(PART_START + part_sectors) // 2048) * 2048
        sys_sectors = sys_len // SECTOR
        total_sectors = sys_start + sys_sectors + 34
    spc = 8 if part_sectors * SECTOR >= (512 << 20) else 1
    rsvd, nfats = 32, 2
    tmp2 = (256 * spc + nfats) // 2
    fatsz = (part_sectors - rsvd + tmp2 - 1) // tmp2
    data_start = rsvd + nfats * fatsz
    nclusters = (part_sectors - data_start) // spc
    if nclusters < 65525:
        raise SystemExit("image too small for FAT32; use --size 64 or more")
    csize = spc * SECTOR

    # allocate clusters: directories first (sized to their entries), then files
    nxt = [2]
    fat = {}

    def alloc(nbytes):
        n = max(1, -(-nbytes // csize))
        first = nxt[0]
        for i in range(n):
            fat[first + i] = first + i + 1 if i < n - 1 else 0x0FFFFFFF
        nxt[0] += n
        if nxt[0] - 2 > nclusters:
            raise SystemExit("files do not fit; use a larger --size")
        return first

    dirs = [n for n in walk(root) if n.is_dir]
    for d in dirs:  # entry count does not depend on cluster numbers
        d.cluster = alloc(len(dir_entries(d, None if d is root else 0)) + 32)
    for f in walk(root):
        if not f.is_dir:
            f.cluster = alloc(f.size) if f.size else 0

    part_off = PART_START * SECTOR
    with open(out, "wb") as img:
        img.truncate(total_sectors * SECTOR)
        # boot sector, FSInfo, backups
        bs = bytearray(512)
        bs[0:3] = b"\xEB\x58\x90"
        bs[3:11] = b"SONAR   "
        struct.pack_into("<HBHBHHBHHHII", bs, 11, SECTOR, spc, rsvd, nfats, 0, 0, 0xF8, 0, 63, 255,
                         PART_START, part_sectors)
        struct.pack_into("<IHHIHH", bs, 36, fatsz, 0, 0, 2, 1, 6)
        struct.pack_into("<BBBI11s8s", bs, 64, 0x80, 0, 0x29, zlib.crc32(out.encode()) & 0xFFFFFFFF,
                         b"SONAR BOOT ", b"FAT32   ")
        bs[510:512] = b"\x55\xAA"
        fsinfo = bytearray(512)
        struct.pack_into("<I", fsinfo, 0, 0x41615252)
        struct.pack_into("<III", fsinfo, 484, 0x61417272, nclusters - (nxt[0] - 2), nxt[0])
        struct.pack_into("<I", fsinfo, 508, 0xAA550000)
        for base in (0, 6):
            img.seek(part_off + base * SECTOR)
            img.write(bs)
            img.write(fsinfo)
        # FATs
        fatbuf = bytearray(4 * nxt[0])
        struct.pack_into("<II", fatbuf, 0, 0x0FFFFFF8, 0x0FFFFFFF)
        for c, v in fat.items():
            struct.pack_into("<I", fatbuf, 4 * c, v)
        for i in range(nfats):
            img.seek(part_off + (rsvd + i * fatsz) * SECTOR)
            img.write(fatbuf)

        def coff(c):
            return part_off + (data_start + (c - 2) * spc) * SECTOR

        def parent_of(target):
            for d in dirs:
                if target in d.children:
                    return d
            return None

        for d in dirs:
            p = parent_of(d)
            img.seek(coff(d.cluster))
            img.write(dir_entries(d, None if d is root else (0 if p is root else p.cluster)))
        for f in walk(root):
            if f.is_dir or not f.size:
                continue
            img.seek(coff(f.cluster))
            with open(f.source, "rb") as src:
                src.seek(f.offset)
                left = f.size
                while left:
                    buf = src.read(min(left, 64 << 20))
                    if not buf:
                        raise SystemExit("%s: short read" % f.source)
                    img.write(buf)
                    left -= len(buf)

        # GPT
        disk_guid, part_guid = uuid.uuid4(), uuid.uuid4()
        last = total_sectors - 1
        pe = bytearray(128 * 128)
        name = "EFI system partition".encode("utf-16-le")
        struct.pack_into("<16s16sQQQ", pe, 0, ESP_TYPE.bytes_le, part_guid.bytes_le, PART_START,
                         PART_START + part_sectors - 1, 0)
        pe[56:56 + len(name)] = name
        if system:
            name2 = "Sonar system".encode("utf-16-le")
            struct.pack_into("<16s16sQQQ", pe, 128, SONAR_TYPE.bytes_le, uuid.uuid4().bytes_le,
                             sys_start, sys_start + sys_sectors - 1, 0)
            pe[128 + 56:128 + 56 + len(name2)] = name2
            with open(system, "rb") as src:
                src.seek(sys_off)
                img.seek(sys_start * SECTOR)
                left = sys_len
                while left:
                    buf = src.read(min(left, 64 << 20))
                    if not buf:
                        raise SystemExit("%s: short read" % system)
                    img.write(buf)
                    left -= len(buf)
        pe_crc = zlib.crc32(pe) & 0xFFFFFFFF

        def header(cur, backup, entries_lba):
            h = bytearray(92)
            struct.pack_into("<8sIIIIQQQQ16sQIII", h, 0, b"EFI PART", 0x00010000, 92, 0, 0, cur, backup,
                             34, last - 33, disk_guid.bytes_le, entries_lba, 128, 128, pe_crc)
            struct.pack_into("<I", h, 16, zlib.crc32(h) & 0xFFFFFFFF)
            return bytes(h) + bytes(SECTOR - 92)

        mbr = bytearray(512)
        struct.pack_into("<B3sB3sII", mbr, 446, 0, b"\x00\x02\x00", 0xEE, b"\xFF\xFF\xFF", 1,
                         min(total_sectors - 1, 0xFFFFFFFF))
        mbr[510:512] = b"\x55\xAA"
        img.seek(0)
        img.write(mbr)
        img.write(header(1, last, 2))
        img.write(pe)
        img.seek((last - 32) * SECTOR)
        img.write(pe)
        img.write(header(last, 1, last - 32))
    print("%s: %d MiB, FAT32 EFI system partition, %d file(s), %.1f MiB of data%s" %
          (out, size_mib, sum(1 for n in walk(root) if not n.is_dir), data_bytes / 2**20,
           ", SonarFS system partition of %d MiB" % (sys_len >> 20) if system else ""))


def walk(n):
    yield n
    for c in n.children:
        yield from walk(c)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("out")
    ap.add_argument("--size", type=int, help="image size in MiB (default: fit the files)")
    ap.add_argument("--system", metavar="SYSTEM.img",
                    help="add a SonarFS system partition from this image (kept across boots)")
    ap.add_argument("files", nargs="+", metavar="DEST=SOURCE")
    a = ap.parse_args()
    files = []
    for spec in a.files:
        dest, eq, src = spec.partition("=")
        if not eq or not os.path.isfile(src):
            sys.exit("bad file argument (want DEST=SOURCE with an existing file): " + spec)
        files.append((dest, src))
    build(a.out, a.size, files, a.system)


if __name__ == "__main__":
    main()
