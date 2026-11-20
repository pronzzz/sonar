# SonarFS on-disk format (version 1)

SonarFS is Sonar's own file system. Two independent implementations read and
write it: the kernel's (`kernel/fs/sonarfs.c`) and the host tool
(`tools/sonarfs.py`), which is used to build disk images and to check the
kernel's work (`sonarfs.py fsck`).

All integers are little-endian. The block size is 4096 bytes.

## Disk layout

A SonarFS disk normally has a GPT partition table (UEFI specification,
chapter 5) with one partition of type
`5a0e7a12-50fa-4f53-9d3e-536f6e617246` (a fixed, Sonar-specific GUID). The
kernel names the partition `<disk>p1`, for example `vda1` or `boot0p1`.
A bare volume without a partition table is accepted too.

Inside the volume:

| Blocks | Contents |
| --- | --- |
| 0 | superblock |
| `bitmap_start` … | block allocation bitmap, 1 bit per block (1 = used) |
| `inode_start` … | inode table, 128-byte inodes, 32 per block |
| `data_start` … | file and directory data |

`mkfs` creates `max(256, min(65536, blocks / 16))` inodes, rounded up to a
whole block. Bitmap bits past the end of the volume are set, so they are
never allocated.

## Superblock (block 0, 176 bytes used)

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 4 | `version` (1) |
| 4 | 4 | `flags`: bit 0 = the volume has a journal |
| 8 | 8 | `magic`, the ASCII bytes `SONARFS1` |
| 16 | 4 | `block_size` (4096) |
| 20 | 4 | `inode_size` (128) |
| 24 | 8 | `total_blocks` |
| 32 | 8 | `inode_count` |
| 40 | 8 | `bitmap_start` |
| 48 | 8 | `bitmap_blocks` |
| 56 | 8 | `inode_start` |
| 64 | 8 | `inode_blocks` |
| 72 | 8 | `data_start` |
| 80 | 8 | `free_blocks` |
| 88 | 8 | `free_inodes` |
| 96 | 8 | `created` (seconds since 1970, UTC) |
| 104 | 8 | `mtime` (last superblock write) |
| 112 | 32 | `label`, NUL-padded |
| 144 | 16 | `uuid` |
| 160 | 8 | `journal_start`, the journal's first block (flag bit 0) |
| 168 | 8 | `journal_blocks` (64) |

Volumes written before the journal existed have zeros at 160–175 and flag
bit 0 clear; the kernel adds a journal the first time it mounts one
writable (64 contiguous free blocks, marked used in the bitmap).

## Inodes (128 bytes)

Inode numbers start at 1; inode 1 is the root directory. Inode *n* is at
byte `n * 128` of the inode table (the first slot, inode 0, is never used). A `type` of 0 means free.

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 2 | `type`: 1 file, 2 directory |
| 2 | 2 | `links` |
| 4 | 4 | `flags` |
| 8 | 8 | `size` in bytes |
| 16 | 8 | `ctime` |
| 24 | 8 | `mtime` |
| 32 | 4 | `nextents`, the total number of extents |
| 36 | 4 | `ext_block`, the first overflow block (0 if none) |
| 40 | 80 | 5 inline extents |
| 120 | 8 | reserved |

An **extent** is 16 bytes: `start` (u64 block number), `len` (u32 blocks)
and 4 reserved bytes. A file's data is its extents' blocks in order. Files
are allocated in as few extents as possible, so large files such as model
weights stay contiguous and read at full disk speed.

Extents past the first five go into **overflow blocks**. Each holds a
16-byte header (`next` u64 block, `count` u32, 4 reserved) and up to 255
extents. `next` chains to the following overflow block, or is 0.

## Directories

A directory is a file of 64-byte entries:

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 4 | `ino` (0 = unused slot) |
| 4 | 1 | `type` (1 file, 2 directory) |
| 5 | 1 | `name_len` |
| 6 | 58 | `name`, not NUL-terminated when 58 bytes long |

Every directory begins with `.` and `..`. Names are at most 58 bytes and
cannot contain `/`.

## Journal

Every operation that changes the volume (creating, writing, renaming or
deleting a file or directory) is one transaction for its metadata: the
superblock, bitmap blocks, inodes, extent overflow blocks and directory
blocks it changes. File contents are not journaled.

The journal is `journal_blocks` (64) blocks starting at `journal_start`:

| Block | Contents |
| --- | --- |
| 0 | header: `magic` (8 bytes, `SNRJRNL1` while it holds a committed transaction), `seq` (u64), `count` (u32), `crc` (u32), then `count` u64 sector numbers (512-byte sectors from the start of the volume's device) |
| 1 … | the new contents of those sectors, 512 bytes each, in the same order |

`crc` is the CRC-32 (the zlib/IEEE polynomial) of the sector numbers
followed by the sector contents. A transaction holds at most 504 sectors;
a larger operation is committed in parts.

The kernel keeps a transaction's sectors in memory until the operation
ends, then:

1. flushes the disk, so the operation's file contents are stored first;
2. writes the sectors to blocks 1…, flushes, writes the header (the
   commit record) and flushes again;
3. writes each sector to its place, flushes, and zeroes the header.

At mount, a header with the magic and a matching `crc` means step 3 may
not have finished; the kernel (and `sonarfs.py` before changing an image)
writes the sectors to their places again and zeroes the header. A header
without a valid `crc` was never committed, so the operation did not
happen. Either way the metadata is consistent after a power cut. What can
still be lost is the contents of a file being written at that moment (its
size and blocks are consistent, but new data in place of old may be
partly written).

`jcrash FILE TEXT [N]` tests this: it writes FILE and cuts the power right
after the operation's Nth commit record.

## Consistency checks

`sonarfs.py fsck IMAGE` walks the tree from the root and reports:
- blocks outside the data area, or used by two files;
- directory entries that point to free inodes, or directories linked twice;
- sizes larger than the allocated blocks, and wrong extent counts;
- missing or wrong `.` and `..` entries;
- entry types that don't match the inode type.

The kernel's `fstest` command exercises the implementation (create, write,
append, rename, delete, verify) inside `/tmp/fstest`.
