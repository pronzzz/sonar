#!/usr/bin/env python3
"""Read files from the FAT32 EFI system partition of a disk image.

  fatcat.py IMAGE /path/in/partition      print a file
  fatcat.py IMAGE /dir --ls               list a directory

Used to read \\sonar\\boot.log (the UEFI loader's log) after booting a VM,
without mounting the image. FAT layout: Microsoft "FAT32 File System
Specification" (2000); long names are matched case-insensitively.
"""
import struct
import sys


def find_esp(img):
    """Byte offset of the first partition (GPT or MBR), or 0 for a bare volume."""
    img.seek(512)
    hdr = img.read(92)
    if hdr[:8] == b"EFI PART":
        lba, = struct.unpack_from("<Q", hdr, 72)
        img.seek(lba * 512)
        ent = img.read(128)
        first, = struct.unpack_from("<Q", ent, 32)
        return first * 512
    img.seek(0)
    mbr = img.read(512)
    if mbr[510:512] == b"\x55\xaa" and mbr[0x1c2] in (0x0b, 0x0c, 0xef):
        return struct.unpack_from("<I", mbr, 0x1c6)[0] * 512
    return 0


class Fat:
    def __init__(self, img, base):
        self.img, self.base = img, base
        img.seek(base)
        bs = img.read(512)
        self.bps, = struct.unpack_from("<H", bs, 11)
        self.spc = bs[13]
        rsvd, = struct.unpack_from("<H", bs, 14)
        nfats = bs[16]
        fatsz, = struct.unpack_from("<I", bs, 36)
        self.root, = struct.unpack_from("<I", bs, 44)
        self.fat_off = base + rsvd * self.bps
        self.data_off = base + (rsvd + nfats * fatsz) * self.bps
        self.csize = self.spc * self.bps

    def chain(self, c):
        while 2 <= c < 0x0FFFFFF8:
            yield c
            self.img.seek(self.fat_off + 4 * c)
            c = struct.unpack("<I", self.img.read(4))[0] & 0x0FFFFFFF

    def read_chain(self, c, size=None):
        out = bytearray()
        for cl in self.chain(c):
            self.img.seek(self.data_off + (cl - 2) * self.csize)
            out += self.img.read(self.csize)
            if size is not None and len(out) >= size:
                break
        return bytes(out[:size] if size is not None else out)

    def entries(self, c):
        data = self.read_chain(c)
        lfn = []
        for i in range(0, len(data), 32):
            e = data[i:i + 32]
            if e[0] == 0:
                break
            if e[0] == 0xE5:
                lfn = []
                continue
            if e[11] == 0x0F:
                part = e[1:11] + e[14:26] + e[28:32]
                lfn.insert(0, part.decode("utf-16-le", "replace").split("\x00")[0].rstrip("￿"))
                continue
            short = e[0:8].decode("ascii", "replace").rstrip() + (
                "." + e[8:11].decode("ascii", "replace").rstrip() if e[8:11].strip() else "")
            name = "".join(lfn) if lfn else short
            lfn = []
            if e[11] & 0x08:
                continue  # volume label
            hi, = struct.unpack_from("<H", e, 20)
            lo, = struct.unpack_from("<H", e, 26)
            size, = struct.unpack_from("<I", e, 28)
            yield name, bool(e[11] & 0x10), hi << 16 | lo, size

    def lookup(self, path):
        c, is_dir, size = self.root, True, 0
        for part in [p for p in path.replace("\\", "/").split("/") if p]:
            for name, d, cl, sz in self.entries(c):
                if name.lower() == part.lower():
                    c, is_dir, size = cl, d, sz
                    break
            else:
                sys.exit(f"fatcat: {path}: not found")
        return c, is_dir, size


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    with open(sys.argv[1], "rb") as img:
        fat = Fat(img, find_esp(img))
        c, is_dir, size = fat.lookup(sys.argv[2])
        if is_dir or "--ls" in sys.argv:
            for name, d, _, sz in fat.entries(c):
                print(f"{'d' if d else '-'} {sz:12d} {name}")
        else:
            sys.stdout.buffer.write(fat.read_chain(c, size))


if __name__ == "__main__":
    main()
