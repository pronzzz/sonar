/*
 * SonarFS: Sonar's native file system (format described in docs/sonarfs.md).
 *
 *   block 0             superblock
 *   bitmap_start..      block allocation bitmap (1 bit per block)
 *   inode_start..       inode table (128-byte inodes; inode 1 = root)
 *   data_start..        file and directory data
 *
 * Files are mapped by extents (start, length) so large files such as model
 * weights stay contiguous and read at full disk speed. Directories are
 * files of 64-byte entries. Metadata is written through on every change.
 */
#include "arch.h"
#include "fs.h"
#include "irq.h"
#include "kernel.h"
#include "mm.h"
#include "string.h"

#define BS 4096u
#define SPB (BS / BLK_SECTOR)
#define ISZ 128u
#define IPB (BS / ISZ)
#define DE_SIZE 64u
#define NAME_MAX FS_NAME_MAX
#define INLINE_EXT 5
#define OVF_EXT 255
#define ROOT_INO 1
#define MAX_FDS 32

struct sb_disk {
    u32 version, flags;
    char magic[8];
    u32 block_size, inode_size;
    u64 total_blocks, inode_count;
    u64 bitmap_start, bitmap_blocks;
    u64 inode_start, inode_blocks;
    u64 data_start;
    u64 free_blocks, free_inodes;
    u64 created, mtime;
    char label[32];
    u8 uuid[16];
    u64 journal_start, journal_blocks; /* SB_JOURNAL: the metadata journal */
};
_Static_assert(sizeof(struct sb_disk) == 176, "superblock layout");
#define SB_JOURNAL 1u /* flags */

/* Metadata journal: block 0 is the header (commit record), blocks 1.. hold
 * the logged 512-byte sectors in order. */
#define JBLOCKS 64
#define JMAX 504 /* sectors per transaction: (BS - 24) / 8, rounded down */
struct jheader {
    char magic[8]; /* "SNRJRNL1" when it holds a committed transaction */
    u64 seq;
    u32 count, crc;
    u64 lba[JMAX];
};
_Static_assert(sizeof(struct jheader) <= BS, "journal header");

struct extent {
    u64 start;
    u32 len;
    u32 reserved;
};

struct inode_disk {
    u16 type, links;
    u32 flags;
    u64 size, ctime, mtime;
    u32 nextents, ext_block;
    struct extent ext[INLINE_EXT];
    u64 reserved;
};
_Static_assert(sizeof(struct inode_disk) == ISZ, "inode layout");

struct ovf_block {
    u64 next;
    u32 count, reserved;
    struct extent ext[OVF_EXT];
};
_Static_assert(sizeof(struct ovf_block) == BS, "extent block layout");

struct dirent_disk {
    u32 ino;
    u8 type, name_len;
    char name[NAME_MAX];
};
_Static_assert(sizeof(struct dirent_disk) == DE_SIZE, "dirent layout");

/* In-memory inode with its full extent list. */
struct inode {
    u32 ino;
    struct inode_disk d;
    struct extent *ext;
    u32 next, cap;
};

static struct {
    struct blkdev *dev;
    bool mounted, ram;
    struct sb_disk sb;
    u8 *bitmap;
    u64 dirty_lo, dirty_hi; /* bitmap blocks to write back */
    u32 inode_hint;
    /* metadata sectors written since the last commit (journaled volumes) */
    bool journal;
    u64 jseq;
    int ntx;
    u64 *tx_lba;
    u8 *tx_data; /* JMAX x 512 */
} fs;

static struct {
    bool used;
    u32 ino;
    int flags;
    u64 pos;
} fds[MAX_FDS];

static char cwd[FS_PATH_MAX] = "/";

const char *fs_strerror(int e)
{
    switch (e) {
    case FS_OK: return "success";
    case FS_ENOENT: return "no such file or directory";
    case FS_EEXIST: return "already exists";
    case FS_ENOTDIR: return "not a directory";
    case FS_EISDIR: return "is a directory";
    case FS_ENOSPC: return "no space left on device";
    case FS_EIO: return "I/O error";
    case FS_EINVAL: return "invalid argument";
    case FS_ENOTEMPTY: return "directory not empty";
    case FS_EROFS: return "read-only file system";
    case FS_ENAMETOOLONG: return "name too long";
    case FS_EBADF: return "bad file descriptor";
    case FS_EMFILE: return "too many open files";
    case FS_ENOMEM: return "out of memory";
    case FS_ENOFS: return "no file system mounted";
    case FS_EBUSY: return "busy";
    }
    return "unknown error";
}

static u64 now(void)
{
    u64 t = rtc_epoch();
    return t;
}

/* ------------------------------------------------------------ block I/O */
/*
 * On a journaled volume, metadata writes (superblock, bitmap, inodes,
 * extent blocks, directories) collect in memory until the operation ends
 * (sync_meta). tx_commit then logs them to the journal with a checksummed
 * commit record, writes them to their places and clears the journal; a
 * volume mounted after a crash replays a committed journal, so an
 * operation's metadata changes land completely or not at all. File
 * contents are written directly, before the commit (like ext3's "ordered"
 * mode); they are not journaled.
 */
static u32 crc32(u32 c, const u8 *p, size_t n)
{
    c = ~c;
    while (n--) {
        c ^= *p++;
        for (int k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xedb88320u & (u32)-(i32)(c & 1));
    }
    return ~c;
}

static bool tx_commit(void);
int fs_crash_test; /* test hook: power off at this commit (counting down) */

static int tx_find(u64 lba)
{
    for (int i = 0; i < fs.ntx; i++)
        if (fs.tx_lba[i] == lba)
            return i;
    return -1;
}

/* Sectors read from disk, with this transaction's pending writes applied. */
static bool dev_read(u64 lba, u32 count, void *buf)
{
    if (!blk_read(fs.dev, lba, count, buf))
        return false;
    for (int i = 0; i < fs.ntx; i++)
        if (fs.tx_lba[i] >= lba && fs.tx_lba[i] < lba + count)
            memcpy((u8 *)buf + (fs.tx_lba[i] - lba) * BLK_SECTOR, fs.tx_data + (size_t)i * BLK_SECTOR,
                   BLK_SECTOR);
    return true;
}

static bool dev_read_bytes(u64 off, void *buf, u64 len)
{
    if (!blk_read_bytes(fs.dev, off, buf, len))
        return false;
    for (int i = 0; i < fs.ntx; i++) {
        u64 s = fs.tx_lba[i] * BLK_SECTOR, e = s + BLK_SECTOR;
        u64 lo = MAX(s, off), hi = MIN(e, off + len);
        if (lo < hi)
            memcpy((u8 *)buf + (lo - off), fs.tx_data + (size_t)i * BLK_SECTOR + (lo - s), hi - lo);
    }
    return true;
}

/* A metadata write: journaled, or straight to the disk. */
static bool meta_write(u64 lba, u32 count, const void *buf)
{
    if (!fs.journal)
        return blk_write(fs.dev, lba, count, buf);
    for (u32 k = 0; k < count; k++) {
        int i = tx_find(lba + k);
        if (i < 0) {
            if (fs.ntx == JMAX && !tx_commit()) /* a very large operation: commit part of it */
                return false;
            i = fs.ntx++;
            fs.tx_lba[i] = lba + k;
        }
        memcpy(fs.tx_data + (size_t)i * BLK_SECTOR, (const u8 *)buf + (size_t)k * BLK_SECTOR,
               BLK_SECTOR);
    }
    return true;
}

/* A file-data write: straight to the disk, replacing any pending metadata
 * for the same sectors (blocks freed and reused within one operation). */
static bool data_write(u64 lba, u32 count, const void *buf)
{
    for (int i = 0; i < fs.ntx;) {
        if (fs.tx_lba[i] >= lba && fs.tx_lba[i] < lba + count) {
            fs.ntx--;
            fs.tx_lba[i] = fs.tx_lba[fs.ntx];
            memcpy(fs.tx_data + (size_t)i * BLK_SECTOR, fs.tx_data + (size_t)fs.ntx * BLK_SECTOR,
                   BLK_SECTOR);
        } else {
            i++;
        }
    }
    return blk_write(fs.dev, lba, count, buf);
}

static bool bread(u64 blk, void *buf) { return dev_read(blk * SPB, SPB, buf); }
static bool bwrite(u64 blk, const void *buf) { return meta_write(blk * SPB, SPB, buf); }

static bool tx_commit(void)
{
    if (!fs.journal || !fs.ntx)
        return true;
    struct blkdev *d = fs.dev;
    u64 j = fs.sb.journal_start * SPB;
    struct jheader *h = kzalloc(BS);
    if (!h)
        return false;
    /* 1. file data written so far reaches the disk before the commit */
    bool ok = blk_flush(d);
    /* 2. the logged sectors, then the commit record */
    ok = ok && blk_write(d, j + SPB, (u32)fs.ntx, fs.tx_data);
    memcpy(h->magic, "SNRJRNL1", 8);
    h->seq = ++fs.jseq;
    h->count = (u32)fs.ntx;
    memcpy(h->lba, fs.tx_lba, sizeof(u64) * (size_t)fs.ntx);
    h->crc = crc32(crc32(0, (const u8 *)h->lba, sizeof(u64) * (size_t)fs.ntx), fs.tx_data,
                   (size_t)fs.ntx * BLK_SECTOR);
    ok = ok && blk_flush(d) && blk_write(d, j, SPB, h) && blk_flush(d);
    if (fs_crash_test > 0 && ok && --fs_crash_test == 0) { /* test hook: power is cut before anything is in place */
        printk("fs: crash test: powering off after the commit record\n");
        psci_system_off();
    }
    /* 3. into place */
    for (int i = 0; ok && i < fs.ntx; i++)
        ok = blk_write(d, fs.tx_lba[i], 1, fs.tx_data + (size_t)i * BLK_SECTOR);
    /* 4. done: clear the record (replaying it again would be harmless) */
    ok = ok && blk_flush(d);
    memset(h, 0, BS);
    ok = ok && blk_write(d, j, SPB, h);
    kfree(h);
    fs.ntx = 0;
    return ok;
}

/* At mount: apply a transaction that was committed but maybe not written
 * into place. Returns the number of sectors replayed, or -1 on error. */
static int journal_replay(struct blkdev *d, const struct sb_disk *sb)
{
    u64 j = sb->journal_start * SPB;
    struct jheader *h = kmalloc(BS);
    u8 *data = kmalloc((size_t)JMAX * BLK_SECTOR);
    int r = 0;
    if (!h || !data || !blk_read(d, j, SPB, h)) {
        r = -1;
    } else if (!memcmp(h->magic, "SNRJRNL1", 8) && h->count && h->count <= JMAX &&
               blk_read(d, j + SPB, h->count, data) &&
               h->crc == crc32(crc32(0, (const u8 *)h->lba, sizeof(u64) * h->count), data,
                               (size_t)h->count * BLK_SECTOR)) {
        bool ok = true;
        u32 n = h->count;
        for (u32 i = 0; ok && i < n; i++)
            ok = h->lba[i] < sb->total_blocks * SPB &&
                 blk_write(d, h->lba[i], 1, data + (size_t)i * BLK_SECTOR);
        memset(h, 0, BS);
        ok = ok && blk_flush(d) && blk_write(d, j, SPB, h) && blk_flush(d);
        r = ok ? (int)n : -1;
    }
    kfree(h);
    kfree(data);
    return r;
}

static bool write_sb(void)
{
    u8 *b = kzalloc(BS);
    if (!b)
        return false;
    fs.sb.mtime = now();
    memcpy(b, &fs.sb, sizeof(fs.sb));
    bool ok = bwrite(0, b);
    kfree(b);
    return ok;
}

/* Write back modified bitmap blocks and the superblock counters. */
static int sync_meta(void)
{
    bool ok = true;
    if (fs.dirty_hi >= fs.dirty_lo) {
        for (u64 b = fs.dirty_lo; b <= fs.dirty_hi; b++)
            ok &= bwrite(fs.sb.bitmap_start + b, fs.bitmap + b * BS);
        fs.dirty_lo = ~0ULL;
        fs.dirty_hi = 0;
    }
    ok &= write_sb();
    ok &= tx_commit();
    return ok ? FS_OK : FS_EIO;
}

static inline bool bm_test(u64 b) { return fs.bitmap[b >> 3] & (1u << (b & 7)); }

static void bm_set(u64 b, bool used)
{
    u8 *p = &fs.bitmap[b >> 3];
    u8 m = (u8)(1u << (b & 7));
    if (used == !!(*p & m))
        return;
    if (used) {
        *p |= m;
        fs.sb.free_blocks--;
    } else {
        *p &= (u8)~m;
        fs.sb.free_blocks++;
    }
    u64 blk = (b >> 3) / BS;
    if (blk < fs.dirty_lo)
        fs.dirty_lo = blk;
    if (blk > fs.dirty_hi)
        fs.dirty_hi = blk;
}

/* Allocate up to `want` contiguous blocks, preferring to start at `near`.
 * Returns the number allocated (0 when the disk is full). */
static u64 alloc_run(u64 want, u64 near, u64 *start)
{
    u64 total = fs.sb.total_blocks;
    if (near >= fs.sb.data_start && near < total && !bm_test(near)) {
        u64 n = 0;
        while (n < want && near + n < total && !bm_test(near + n))
            n++;
        for (u64 i = 0; i < n; i++)
            bm_set(near + i, true);
        *start = near;
        return n;
    }
    u64 best = 0, best_len = 0;
    const u64 *words = (const u64 *)fs.bitmap;
    for (u64 b = fs.sb.data_start; b < total;) {
        if ((b & 63) == 0 && words[b >> 6] == ~0ULL) {
            b += 64;
            continue;
        }
        if (bm_test(b)) {
            b++;
            continue;
        }
        u64 s = b;
        while (b < total && !bm_test(b) && b - s < want)
            b++;
        u64 len = b - s;
        if (len > best_len) {
            best = s;
            best_len = len;
        }
        if (len >= want)
            break;
    }
    for (u64 i = 0; i < best_len; i++)
        bm_set(best + i, true);
    *start = best;
    return best_len;
}

static void free_run(u64 start, u64 len)
{
    for (u64 i = 0; i < len; i++)
        if (start + i >= fs.sb.data_start && start + i < fs.sb.total_blocks)
            bm_set(start + i, false);
}

/* ------------------------------------------------------------ inodes */
static bool iread_disk(u32 ino, struct inode_disk *d)
{
    u64 byte = fs.sb.inode_start * BS + (u64)ino * ISZ;
    return dev_read_bytes(byte, d, ISZ);
}

static bool iwrite_disk(u32 ino, const struct inode_disk *d)
{
    u64 byte = fs.sb.inode_start * BS + (u64)ino * ISZ;
    u8 sec[BLK_SECTOR] __attribute__((aligned(16)));
    u64 lba = byte / BLK_SECTOR;
    if (!dev_read(lba, 1, sec))
        return false;
    memcpy(sec + byte % BLK_SECTOR, d, ISZ);
    return meta_write(lba, 1, sec);
}

static void iput(struct inode *ip)
{
    kfree(ip->ext);
    ip->ext = NULL;
}

static int iget(u32 ino, struct inode *ip)
{
    memset(ip, 0, sizeof(*ip));
    if (ino == 0 || ino >= fs.sb.inode_count)
        return FS_EINVAL;
    ip->ino = ino;
    if (!iread_disk(ino, &ip->d))
        return FS_EIO;
    if (ip->d.type == FT_NONE)
        return FS_ENOENT;
    u32 n = ip->d.nextents;
    ip->cap = n < 8 ? 8 : n;
    ip->ext = kmalloc(sizeof(struct extent) * ip->cap);
    if (!ip->ext)
        return FS_ENOMEM;
    u32 k = 0;
    for (; k < n && k < INLINE_EXT; k++)
        ip->ext[k] = ip->d.ext[k];
    u64 blk = ip->d.ext_block;
    struct ovf_block *ob = blk ? kmalloc(BS) : NULL;
    while (k < n && blk) {
        if (!ob || !bread(blk, ob) || ob->count > OVF_EXT) {
            kfree(ob);
            iput(ip);
            return FS_EIO;
        }
        for (u32 j = 0; j < ob->count && k < n; j++)
            ip->ext[k++] = ob->ext[j];
        blk = ob->next;
    }
    kfree(ob);
    ip->next = k;
    return k == n ? FS_OK : FS_EIO;
}

/* Free the overflow extent chain of an on-disk inode. */
static void free_ovf_chain(u64 blk)
{
    struct ovf_block *ob = kmalloc(BS);
    while (blk && ob) {
        if (!bread(blk, ob))
            break;
        u64 next = ob->next;
        free_run(blk, 1);
        blk = next;
    }
    kfree(ob);
}

/* Write the inode, storing extents inline and in overflow blocks. */
static int isave(struct inode *ip)
{
    struct inode_disk *d = &ip->d;
    free_ovf_chain(d->ext_block);
    d->ext_block = 0;
    d->nextents = ip->next;
    memset(d->ext, 0, sizeof(d->ext));
    for (u32 k = 0; k < ip->next && k < INLINE_EXT; k++)
        d->ext[k] = ip->ext[k];
    if (ip->next > INLINE_EXT) {
        u32 left = ip->next - INLINE_EXT, k = INLINE_EXT;
        u64 nblk = (left + OVF_EXT - 1) / OVF_EXT;
        u64 *blks = kmalloc(sizeof(u64) * nblk);
        struct ovf_block *ob = kzalloc(BS);
        if (!blks || !ob) {
            kfree(blks);
            kfree(ob);
            return FS_ENOMEM;
        }
        for (u64 i = 0; i < nblk; i++) {
            if (alloc_run(1, 0, &blks[i]) != 1) {
                for (u64 j = 0; j < i; j++)
                    free_run(blks[j], 1);
                kfree(blks);
                kfree(ob);
                return FS_ENOSPC;
            }
        }
        for (u64 i = 0; i < nblk; i++) {
            memset(ob, 0, BS);
            ob->count = left < OVF_EXT ? left : OVF_EXT;
            for (u32 j = 0; j < ob->count; j++)
                ob->ext[j] = ip->ext[k++];
            left -= ob->count;
            ob->next = i + 1 < nblk ? blks[i + 1] : 0;
            if (!bwrite(blks[i], ob)) {
                kfree(blks);
                kfree(ob);
                return FS_EIO;
            }
        }
        d->ext_block = (u32)blks[0];
        kfree(blks);
        kfree(ob);
    }
    return iwrite_disk(ip->ino, d) ? FS_OK : FS_EIO;
}

static u64 iblocks(const struct inode *ip)
{
    u64 n = 0;
    for (u32 k = 0; k < ip->next; k++)
        n += ip->ext[k].len;
    return n;
}

/* Grow the inode's allocation to at least `need` blocks. */
static int igrow(struct inode *ip, u64 need)
{
    u64 have = iblocks(ip);
    while (have < need) {
        u64 near = 0;
        if (ip->next)
            near = ip->ext[ip->next - 1].start + ip->ext[ip->next - 1].len;
        u64 want = need - have, start;
        if (want > 0xffffffffu)
            want = 0xffffffffu;
        u64 got = alloc_run(want, near, &start);
        if (!got)
            return FS_ENOSPC;
        if (ip->next && start == near && (u64)ip->ext[ip->next - 1].len + got <= 0xffffffffu) {
            ip->ext[ip->next - 1].len += (u32)got;
        } else {
            if (ip->next == ip->cap) {
                u32 nc = ip->cap * 2;
                struct extent *ne = krealloc(ip->ext, sizeof(struct extent) * nc);
                if (!ne) {
                    free_run(start, got);
                    return FS_ENOMEM;
                }
                ip->ext = ne;
                ip->cap = nc;
            }
            ip->ext[ip->next].start = start;
            ip->ext[ip->next].len = (u32)got;
            ip->ext[ip->next].reserved = 0;
            ip->next++;
        }
        have += got;
    }
    return FS_OK;
}

/* Release all data blocks (truncate to zero). */
static void itruncate(struct inode *ip)
{
    for (u32 k = 0; k < ip->next; k++)
        free_run(ip->ext[k].start, ip->ext[k].len);
    ip->next = 0;
    ip->d.size = 0;
}

/* Device byte offset of file offset `off`, and contiguous bytes from there. */
static bool imap(const struct inode *ip, u64 off, u64 *dev_off, u64 *run)
{
    u64 fb = off / BS, base = 0;
    for (u32 k = 0; k < ip->next; k++) {
        if (fb < base + ip->ext[k].len) {
            u64 blk = ip->ext[k].start + (fb - base);
            *dev_off = blk * BS + off % BS;
            *run = (base + ip->ext[k].len) * BS - off;
            return true;
        }
        base += ip->ext[k].len;
    }
    return false;
}

static ssize_t ipread(const struct inode *ip, u64 off, void *buf, u64 n)
{
    if (off >= ip->d.size)
        return 0;
    if (n > ip->d.size - off)
        n = ip->d.size - off;
    u8 *out = buf;
    u64 done = 0;
    while (done < n) {
        u64 dev_off, run;
        if (!imap(ip, off + done, &dev_off, &run))
            return FS_EIO;
        u64 c = MIN(run, n - done);
        if (!dev_read_bytes(dev_off, out + done, c))
            return FS_EIO;
        done += c;
    }
    return (ssize_t)n;
}

static ssize_t ipwrite(struct inode *ip, u64 off, const void *buf, u64 n)
{
    if (fs.dev->readonly)
        return FS_EROFS;
    if (off > ip->d.size)
        return FS_EINVAL; /* no holes */
    int r = igrow(ip, (off + n + BS - 1) / BS);
    if (r < 0)
        return r;
    const u8 *in = buf;
    u8 *blk = NULL;
    u64 done = 0;
    while (done < n) {
        u64 pos = off + done, dev_off, run;
        if (!imap(ip, pos, &dev_off, &run)) {
            kfree(blk);
            return FS_EIO;
        }
        u64 c;
        if (pos % BS == 0 && n - done >= BS) {
            /* whole blocks, as many as are contiguous */
            c = MIN(run, n - done) / BS * BS;
            u64 lba = dev_off / BLK_SECTOR;
            u32 cnt = (u32)(c / BLK_SECTOR);
            bool wok = ip->d.type == FT_DIR ? meta_write(lba, cnt, in + done)
                                            : data_write(lba, cnt, in + done);
            if (!wok) {
                kfree(blk);
                return FS_EIO;
            }
        } else {
            /* partial block: read-modify-write */
            if (!blk && !(blk = kmalloc(BS)))
                return FS_ENOMEM;
            u64 bstart = pos - pos % BS, dblk = (dev_off - pos % BS) / BS;
            c = MIN(BS - pos % BS, n - done);
            if (bstart < ip->d.size) {
                if (!bread(dblk, blk)) {
                    kfree(blk);
                    return FS_EIO;
                }
            } else {
                memset(blk, 0, BS);
            }
            memcpy(blk + pos % BS, in + done, c);
            if (!(ip->d.type == FT_DIR ? bwrite(dblk, blk) : data_write(dblk * SPB, SPB, blk))) {
                kfree(blk);
                return FS_EIO;
            }
        }
        done += c;
    }
    kfree(blk);
    if (off + n > ip->d.size)
        ip->d.size = off + n;
    ip->d.mtime = now();
    return (ssize_t)n;
}

static int ialloc(int type, u32 *ino_out)
{
    u32 n = (u32)fs.sb.inode_count;
    for (u32 i = 0; i < n; i++) {
        u32 ino = 2 + (fs.inode_hint + i) % (n - 2);
        struct inode_disk d;
        if (!iread_disk(ino, &d))
            return FS_EIO;
        if (d.type == FT_NONE) {
            memset(&d, 0, sizeof(d));
            d.type = (u16)type;
            d.links = 1;
            d.ctime = d.mtime = now();
            if (!iwrite_disk(ino, &d))
                return FS_EIO;
            fs.sb.free_inodes--;
            fs.inode_hint = ino - 1;
            *ino_out = ino;
            return FS_OK;
        }
    }
    return FS_ENOSPC;
}

static int ifree(struct inode *ip)
{
    itruncate(ip);
    free_ovf_chain(ip->d.ext_block);
    memset(&ip->d, 0, sizeof(ip->d));
    fs.sb.free_inodes++;
    return iwrite_disk(ip->ino, &ip->d) ? FS_OK : FS_EIO;
}

/* ------------------------------------------------------------ directories */
static int dir_lookup(struct inode *dir, const char *name, u32 *ino, int *type, u64 *slot)
{
    size_t nl = strlen(name);
    struct dirent_disk de[BS / DE_SIZE];
    for (u64 off = 0; off < dir->d.size; off += BS) {
        ssize_t r = ipread(dir, off, de, BS);
        if (r < 0)
            return (int)r;
        for (u32 i = 0; i < (u64)r / DE_SIZE; i++) {
            if (de[i].ino && de[i].name_len == nl && !memcmp(de[i].name, name, nl)) {
                *ino = de[i].ino;
                if (type)
                    *type = de[i].type;
                if (slot)
                    *slot = off + i * DE_SIZE;
                return FS_OK;
            }
        }
    }
    return FS_ENOENT;
}

static int dir_add(struct inode *dir, const char *name, u32 ino, int type)
{
    size_t nl = strlen(name);
    if (nl == 0 || nl > NAME_MAX)
        return FS_ENAMETOOLONG;
    struct dirent_disk e;
    memset(&e, 0, sizeof(e));
    e.ino = ino;
    e.type = (u8)type;
    e.name_len = (u8)nl;
    memcpy(e.name, name, nl);
    struct dirent_disk de[BS / DE_SIZE];
    u64 slot = dir->d.size;
    for (u64 off = 0; off < dir->d.size && slot == dir->d.size; off += BS) {
        ssize_t r = ipread(dir, off, de, BS);
        if (r < 0)
            return (int)r;
        for (u32 i = 0; i < (u64)r / DE_SIZE; i++) {
            if (!de[i].ino) {
                slot = off + i * DE_SIZE;
                break;
            }
        }
    }
    ssize_t w = ipwrite(dir, slot, &e, DE_SIZE);
    if (w < 0)
        return (int)w;
    return isave(dir);
}

static int dir_clear_slot(struct inode *dir, u64 slot)
{
    struct dirent_disk e;
    memset(&e, 0, sizeof(e));
    ssize_t w = ipwrite(dir, slot, &e, DE_SIZE);
    if (w < 0)
        return (int)w;
    return isave(dir);
}

static bool dir_empty(struct inode *dir)
{
    struct dirent_disk de[BS / DE_SIZE];
    for (u64 off = 0; off < dir->d.size; off += BS) {
        ssize_t r = ipread(dir, off, de, BS);
        if (r < 0)
            return false;
        for (u32 i = 0; i < (u64)r / DE_SIZE; i++) {
            if (!de[i].ino)
                continue;
            if ((de[i].name_len == 1 && de[i].name[0] == '.') ||
                (de[i].name_len == 2 && de[i].name[0] == '.' && de[i].name[1] == '.'))
                continue;
            return false;
        }
    }
    return true;
}

/* ------------------------------------------------------------ paths */
int fs_abspath(const char *path, char *out, size_t n)
{
    char tmp[FS_PATH_MAX * 2];
    if (!path || !*path)
        path = ".";
    if (path[0] == '/')
        strlcpy(tmp, path, sizeof(tmp));
    else
        snprintf(tmp, sizeof(tmp), "%s/%s", cwd, path);
    /* normalize: split on '/', drop "." and empty, ".." pops */
    char *comp[64];
    int nc = 0;
    char *p = tmp;
    while (*p) {
        while (*p == '/')
            p++;
        if (!*p)
            break;
        char *s = p;
        while (*p && *p != '/')
            p++;
        if (*p)
            *p++ = 0;
        if (!strcmp(s, "."))
            continue;
        if (!strcmp(s, "..")) {
            if (nc)
                nc--;
            continue;
        }
        if (strlen(s) > NAME_MAX)
            return FS_ENAMETOOLONG;
        if (nc == 64)
            return FS_ENAMETOOLONG;
        comp[nc++] = s;
    }
    size_t len = 0;
    out[0] = 0;
    if (!nc) {
        strlcpy(out, "/", n);
        return FS_OK;
    }
    for (int i = 0; i < nc; i++) {
        size_t cl = strlen(comp[i]);
        if (len + 1 + cl + 1 > n || len + 1 + cl + 1 > FS_PATH_MAX)
            return FS_ENAMETOOLONG;
        out[len++] = '/';
        memcpy(out + len, comp[i], cl);
        len += cl;
        out[len] = 0;
    }
    return FS_OK;
}

/* Resolve an absolute normalized path to an inode number. */
static int walk(const char *abs, u32 *ino_out, int *type_out)
{
    u32 ino = ROOT_INO;
    int type = FT_DIR;
    const char *p = abs;
    char name[NAME_MAX + 1];
    while (*p) {
        while (*p == '/')
            p++;
        if (!*p)
            break;
        size_t l = 0;
        while (p[l] && p[l] != '/')
            l++;
        if (type != FT_DIR)
            return FS_ENOTDIR;
        memcpy(name, p, l);
        name[l] = 0;
        p += l;
        struct inode dir;
        int r = iget(ino, &dir);
        if (r < 0)
            return r;
        r = dir_lookup(&dir, name, &ino, &type, NULL);
        iput(&dir);
        if (r < 0)
            return r;
    }
    *ino_out = ino;
    if (type_out)
        *type_out = type;
    return FS_OK;
}

/* Split into parent inode and final name. */
static int walk_parent(const char *path, char *abs, u32 *parent, char *name)
{
    int r = fs_abspath(path, abs, FS_PATH_MAX);
    if (r < 0)
        return r;
    if (!strcmp(abs, "/"))
        return FS_EINVAL;
    char *slash = strrchr(abs, '/');
    strlcpy(name, slash + 1, NAME_MAX + 1);
    char pp[FS_PATH_MAX];
    size_t pl = (size_t)(slash - abs);
    memcpy(pp, abs, pl);
    pp[pl] = 0;
    if (!pl)
        strlcpy(pp, "/", sizeof(pp));
    int type;
    r = walk(pp, parent, &type);
    if (r < 0)
        return r;
    return type == FT_DIR ? FS_OK : FS_ENOTDIR;
}

#define REQUIRE_FS()                                                                               \
    do {                                                                                           \
        if (!fs.mounted)                                                                           \
            return FS_ENOFS;                                                                       \
    } while (0)

const char *fs_cwd(void) { return cwd; }

int fs_chdir(const char *path)
{
    REQUIRE_FS();
    char abs[FS_PATH_MAX];
    int r = fs_abspath(path, abs, sizeof(abs));
    if (r < 0)
        return r;
    u32 ino;
    int type;
    if ((r = walk(abs, &ino, &type)) < 0)
        return r;
    if (type != FT_DIR)
        return FS_ENOTDIR;
    strlcpy(cwd, abs, sizeof(cwd));
    return FS_OK;
}

int fs_stat(const char *path, struct fs_stat *st)
{
    REQUIRE_FS();
    char abs[FS_PATH_MAX];
    int r = fs_abspath(path, abs, sizeof(abs));
    if (r < 0)
        return r;
    u32 ino;
    if ((r = walk(abs, &ino, NULL)) < 0)
        return r;
    struct inode ip;
    if ((r = iget(ino, &ip)) < 0)
        return r;
    st->ino = ino;
    st->type = ip.d.type;
    st->size = ip.d.size;
    st->ctime = ip.d.ctime;
    st->mtime = ip.d.mtime;
    st->links = ip.d.links;
    st->nextents = ip.d.nextents;
    iput(&ip);
    return FS_OK;
}

static int make_node(const char *path, int type, u32 *ino_out)
{
    if (fs.dev->readonly)
        return FS_EROFS;
    char abs[FS_PATH_MAX], name[NAME_MAX + 1];
    u32 pino;
    int r = walk_parent(path, abs, &pino, name);
    if (r < 0)
        return r;
    struct inode dir;
    if ((r = iget(pino, &dir)) < 0)
        return r;
    u32 exist;
    if (dir_lookup(&dir, name, &exist, NULL, NULL) == FS_OK) {
        iput(&dir);
        return FS_EEXIST;
    }
    u32 ino;
    if ((r = ialloc(type, &ino)) < 0) {
        iput(&dir);
        return r;
    }
    if (type == FT_DIR) {
        struct inode nd;
        if ((r = iget(ino, &nd)) < 0) {
            iput(&dir);
            return r;
        }
        nd.d.links = 2;
        r = dir_add(&nd, ".", ino, FT_DIR);
        if (r == FS_OK)
            r = dir_add(&nd, "..", pino, FT_DIR);
        iput(&nd);
        if (r == FS_OK)
            dir.d.links++;
    }
    if (r == FS_OK)
        r = dir_add(&dir, name, ino, type);
    iput(&dir);
    if (r < 0) {
        struct inode bad;
        if (iget(ino, &bad) == FS_OK) {
            ifree(&bad);
            iput(&bad);
        }
    }
    int s = sync_meta();
    if (r == FS_OK && ino_out)
        *ino_out = ino;
    return r < 0 ? r : s;
}

int fs_mkdir(const char *path)
{
    REQUIRE_FS();
    return make_node(path, FT_DIR, NULL);
}

static int remove_node(const char *path, int want_type)
{
    REQUIRE_FS();
    if (fs.dev->readonly)
        return FS_EROFS;
    char abs[FS_PATH_MAX], name[NAME_MAX + 1];
    u32 pino;
    int r = walk_parent(path, abs, &pino, name);
    if (r < 0)
        return r;
    if (!strcmp(name, ".") || !strcmp(name, ".."))
        return FS_EINVAL;
    if (want_type == FT_DIR && !strncmp(cwd, abs, strlen(abs)) &&
        (cwd[strlen(abs)] == 0 || cwd[strlen(abs)] == '/'))
        return FS_EBUSY;
    struct inode dir, ip;
    if ((r = iget(pino, &dir)) < 0)
        return r;
    u32 ino;
    int type;
    u64 slot;
    if ((r = dir_lookup(&dir, name, &ino, &type, &slot)) < 0) {
        iput(&dir);
        return r;
    }
    if ((r = iget(ino, &ip)) < 0) {
        iput(&dir);
        return r;
    }
    if (want_type == FT_FILE && ip.d.type == FT_DIR)
        r = FS_EISDIR;
    else if (want_type == FT_DIR && ip.d.type != FT_DIR)
        r = FS_ENOTDIR;
    else if (want_type == FT_DIR && !dir_empty(&ip))
        r = FS_ENOTEMPTY;
    for (int i = 0; i < MAX_FDS && r == FS_OK; i++)
        if (fds[i].used && fds[i].ino == ino)
            r = FS_EBUSY;
    if (r == FS_OK)
        r = dir_clear_slot(&dir, slot);
    if (r == FS_OK) {
        if (ip.d.type == FT_DIR) {
            dir.d.links--;
            iwrite_disk(dir.ino, &dir.d);
        }
        r = ifree(&ip);
    }
    iput(&ip);
    iput(&dir);
    int s = sync_meta();
    return r < 0 ? r : s;
}

int fs_unlink(const char *path) { return remove_node(path, FT_FILE); }
int fs_rmdir(const char *path) { return remove_node(path, FT_DIR); }

int fs_rename(const char *from, const char *to)
{
    REQUIRE_FS();
    if (fs.dev->readonly)
        return FS_EROFS;
    char afrom[FS_PATH_MAX], ato[FS_PATH_MAX], nfrom[NAME_MAX + 1], nto[NAME_MAX + 1];
    u32 pfrom, pto;
    int r = walk_parent(from, afrom, &pfrom, nfrom);
    if (r < 0)
        return r;
    /* `to` naming an existing directory means "move into it" */
    struct fs_stat st;
    char target[FS_PATH_MAX];
    strlcpy(target, to, sizeof(target));
    if (fs_stat(to, &st) == FS_OK && st.type == FT_DIR) {
        char t2[FS_PATH_MAX];
        if (fs_abspath(to, t2, sizeof(t2)) < 0)
            return FS_ENAMETOOLONG;
        snprintf(target, sizeof(target), "%s/%s", strcmp(t2, "/") ? t2 : "", nfrom);
    }
    if ((r = walk_parent(target, ato, &pto, nto)) < 0)
        return r;
    if (!strcmp(afrom, ato))
        return FS_OK;
    size_t fl = strlen(afrom);
    if (!strncmp(ato, afrom, fl) && ato[fl] == '/')
        return FS_EINVAL; /* directory into itself */
    struct inode dfrom, dto;
    if ((r = iget(pfrom, &dfrom)) < 0)
        return r;
    u32 ino;
    int type;
    u64 slot;
    if ((r = dir_lookup(&dfrom, nfrom, &ino, &type, &slot)) < 0) {
        iput(&dfrom);
        return r;
    }
    if ((r = iget(pto, &dto)) < 0) {
        iput(&dfrom);
        return r;
    }
    u32 old;
    int otype;
    if (dir_lookup(&dto, nto, &old, &otype, NULL) == FS_OK) {
        if (otype == FT_DIR || type == FT_DIR) {
            r = FS_EEXIST;
        } else {
            iput(&dto);
            r = fs_unlink(ato); /* replace an existing file */
            if (r == FS_OK)
                r = iget(pto, &dto);
            if (r < 0) {
                iput(&dfrom);
                return r;
            }
        }
    }
    if (r == FS_OK)
        r = dir_add(&dto, nto, ino, type);
    if (r == FS_OK) {
        iput(&dfrom); /* dto may share the inode: re-read */
        if ((r = iget(pfrom, &dfrom)) == FS_OK)
            r = dir_lookup(&dfrom, nfrom, &ino, &type, &slot);
        if (r == FS_OK)
            r = dir_clear_slot(&dfrom, slot);
    }
    if (r == FS_OK && type == FT_DIR && pfrom != pto) {
        /* fix the moved directory's ".." and the link counts */
        struct inode moved;
        if ((r = iget(ino, &moved)) == FS_OK) {
            u32 dd;
            u64 dslot;
            if (dir_lookup(&moved, "..", &dd, NULL, &dslot) == FS_OK) {
                struct dirent_disk e;
                ipread(&moved, dslot, &e, DE_SIZE);
                e.ino = pto;
                ipwrite(&moved, dslot, &e, DE_SIZE);
                r = isave(&moved);
            }
            iput(&moved);
        }
        iput(&dfrom);
        iput(&dto);
        if (iget(pfrom, &dfrom) == FS_OK) {
            dfrom.d.links--;
            iwrite_disk(pfrom, &dfrom.d);
        }
        if (iget(pto, &dto) == FS_OK) {
            dto.d.links++;
            iwrite_disk(pto, &dto.d);
        }
    }
    iput(&dfrom);
    iput(&dto);
    int s = sync_meta();
    return r < 0 ? r : s;
}

int fs_list(const char *path, struct fs_dirent **out, int *n)
{
    REQUIRE_FS();
    *out = NULL;
    *n = 0;
    char abs[FS_PATH_MAX];
    int r = fs_abspath(path, abs, sizeof(abs));
    if (r < 0)
        return r;
    u32 ino;
    int type;
    if ((r = walk(abs, &ino, &type)) < 0)
        return r;
    if (type != FT_DIR)
        return FS_ENOTDIR;
    struct inode dir;
    if ((r = iget(ino, &dir)) < 0)
        return r;
    int cap = (int)(dir.d.size / DE_SIZE) + 1, cnt = 0;
    struct fs_dirent *arr = kmalloc(sizeof(*arr) * (size_t)cap);
    struct dirent_disk de[BS / DE_SIZE];
    for (u64 off = 0; arr && off < dir.d.size; off += BS) {
        ssize_t rr = ipread(&dir, off, de, BS);
        if (rr < 0) {
            r = (int)rr;
            break;
        }
        for (u32 i = 0; i < (u64)rr / DE_SIZE; i++) {
            if (!de[i].ino)
                continue;
            if ((de[i].name_len == 1 && de[i].name[0] == '.') ||
                (de[i].name_len == 2 && !memcmp(de[i].name, "..", 2)))
                continue;
            arr[cnt].ino = de[i].ino;
            arr[cnt].type = de[i].type;
            memcpy(arr[cnt].name, de[i].name, de[i].name_len);
            arr[cnt].name[de[i].name_len] = 0;
            cnt++;
        }
    }
    iput(&dir);
    if (!arr)
        return FS_ENOMEM;
    if (r < 0) {
        kfree(arr);
        return r;
    }
    /* sort by name (insertion sort; directories are small) */
    for (int i = 1; i < cnt; i++) {
        struct fs_dirent t = arr[i];
        int j = i;
        while (j > 0 && strcmp(arr[j - 1].name, t.name) > 0) {
            arr[j] = arr[j - 1];
            j--;
        }
        arr[j] = t;
    }
    *out = arr;
    *n = cnt;
    return FS_OK;
}

/* ------------------------------------------------------------ files */
int fs_open(const char *path, int flags)
{
    REQUIRE_FS();
    if ((flags & (O_WRITE | O_CREATE | O_TRUNC | O_APPEND)) && fs.dev->readonly)
        return FS_EROFS;
    char abs[FS_PATH_MAX];
    int r = fs_abspath(path, abs, sizeof(abs));
    if (r < 0)
        return r;
    u32 ino;
    int type;
    r = walk(abs, &ino, &type);
    if (r == FS_ENOENT && (flags & O_CREATE))
        r = make_node(abs, FT_FILE, &ino), type = FT_FILE;
    if (r < 0)
        return r;
    if (type == FT_DIR)
        return FS_EISDIR;
    int fd = -1;
    for (int i = 0; i < MAX_FDS; i++) {
        if (!fds[i].used) {
            fd = i;
            break;
        }
    }
    if (fd < 0)
        return FS_EMFILE;
    struct inode ip;
    if ((r = iget(ino, &ip)) < 0)
        return r;
    if (flags & O_TRUNC) {
        itruncate(&ip);
        ip.d.mtime = now();
        r = isave(&ip);
        int s = sync_meta();
        if (r == FS_OK)
            r = s;
    }
    u64 size = ip.d.size;
    iput(&ip);
    if (r < 0)
        return r;
    fds[fd].used = true;
    fds[fd].ino = ino;
    fds[fd].flags = flags;
    fds[fd].pos = (flags & O_APPEND) ? size : 0;
    return fd;
}

static bool fd_ok(int fd) { return fd >= 0 && fd < MAX_FDS && fds[fd].used && fs.mounted; }

ssize_t fs_pread(int fd, u64 off, void *buf, u64 n)
{
    if (!fd_ok(fd))
        return FS_EBADF;
    struct inode ip;
    int r = iget(fds[fd].ino, &ip);
    if (r < 0)
        return r;
    ssize_t got = ipread(&ip, off, buf, n);
    iput(&ip);
    return got;
}

ssize_t fs_read(int fd, void *buf, size_t n)
{
    if (!fd_ok(fd))
        return FS_EBADF;
    ssize_t got = fs_pread(fd, fds[fd].pos, buf, n);
    if (got > 0)
        fds[fd].pos += (u64)got;
    return got;
}

ssize_t fs_write(int fd, const void *buf, size_t n)
{
    if (!fd_ok(fd))
        return FS_EBADF;
    if (!(fds[fd].flags & (O_WRITE | O_APPEND | O_CREATE | O_TRUNC)))
        return FS_EBADF;
    struct inode ip;
    int r = iget(fds[fd].ino, &ip);
    if (r < 0)
        return r;
    if (fds[fd].flags & O_APPEND)
        fds[fd].pos = ip.d.size;
    ssize_t w = ipwrite(&ip, fds[fd].pos, buf, n);
    if (w > 0) {
        fds[fd].pos += (u64)w;
        r = isave(&ip);
        if (r < 0)
            w = r;
    }
    iput(&ip);
    int s = sync_meta();
    return w < 0 ? w : (s < 0 ? s : w);
}

int fs_seek(int fd, u64 off)
{
    if (!fd_ok(fd))
        return FS_EBADF;
    if (off > fs_fsize(fd))
        return FS_EINVAL;
    fds[fd].pos = off;
    return FS_OK;
}

u64 fs_fsize(int fd)
{
    if (!fd_ok(fd))
        return 0;
    struct inode_disk d;
    return iread_disk(fds[fd].ino, &d) ? d.size : 0;
}

int fs_close(int fd)
{
    if (fd < 0 || fd >= MAX_FDS || !fds[fd].used)
        return FS_EBADF;
    fds[fd].used = false;
    return FS_OK;
}

int fs_read_all(const char *path, char **out, size_t *len)
{
    *out = NULL;
    int fd = fs_open(path, O_READ);
    if (fd < 0)
        return fd;
    u64 size = fs_fsize(fd);
    char *buf = kmalloc(size + 1);
    if (!buf) {
        fs_close(fd);
        return FS_ENOMEM;
    }
    ssize_t r = fs_pread(fd, 0, buf, size);
    fs_close(fd);
    if (r < 0) {
        kfree(buf);
        return (int)r;
    }
    buf[r] = 0;
    *out = buf;
    if (len)
        *len = (size_t)r;
    return FS_OK;
}

static int write_with(const char *path, const void *data, size_t len, int flags)
{
    int fd = fs_open(path, flags);
    if (fd < 0)
        return fd;
    ssize_t w = len ? fs_write(fd, data, len) : 0;
    fs_close(fd);
    return w < 0 ? (int)w : FS_OK;
}

int fs_write_all(const char *path, const void *data, size_t len)
{
    return write_with(path, data, len, O_WRITE | O_CREATE | O_TRUNC);
}

int fs_append(const char *path, const void *data, size_t len)
{
    return write_with(path, data, len, O_WRITE | O_CREATE | O_APPEND);
}

/* ------------------------------------------------------------ mkfs / mount */
/* Use the volume's journal, adding one to a volume made without it. Disks
 * held in memory get none (they do not survive power-off anyway). */
static void journal_start(struct blkdev *d)
{
    if (d->readonly || fs.ram || d->mem || (d->parent && d->parent->mem))
        return;
    if (!fs.tx_lba) {
        fs.tx_lba = kmalloc(sizeof(u64) * JMAX);
        fs.tx_data = kmalloc((size_t)JMAX * BLK_SECTOR);
        if (!fs.tx_lba || !fs.tx_data)
            return;
    }
    if (!(fs.sb.flags & SB_JOURNAL)) {
        u64 start;
        u64 got = alloc_run(JBLOCKS, fs.sb.data_start, &start);
        if (got < JBLOCKS) {
            free_run(start, got);
            return; /* no contiguous room: carry on without a journal */
        }
        u8 *z = kzalloc(BS);
        if (!z || !blk_write(d, start * SPB, SPB, z)) {
            kfree(z);
            free_run(start, got);
            return;
        }
        kfree(z);
        fs.sb.journal_start = start;
        fs.sb.journal_blocks = JBLOCKS;
        fs.sb.flags |= SB_JOURNAL;
        if (sync_meta() < 0 || !blk_flush(d))
            return;
        printk("fs: %s: added a metadata journal\n", d->name);
    }
    fs.journal = true;
}

bool fs_probe(struct blkdev *d)
{
    u8 sec[BLK_SECTOR];
    if (d->nsectors < 64 * SPB || !blk_read(d, 0, 1, sec))
        return false;
    const struct sb_disk *sb = (const void *)sec;
    return !memcmp(sb->magic, "SONARFS1", 8) && sb->version == 1 && sb->block_size == BS &&
           sb->inode_size == ISZ;
}

bool fs_mkfs(struct blkdev *d, const char *label, char *err, size_t errlen)
{
    u64 total = d->nsectors / SPB;
    if (d->readonly) {
        snprintf(err, errlen, "%s is read-only", d->name);
        return false;
    }
    if (total < 64) {
        snprintf(err, errlen, "%s is too small (need at least 256 KiB)", d->name);
        return false;
    }
    if (total > 0xffffffffULL)
        total = 0xffffffffULL; /* overflow extent blocks use 32-bit numbers */
    u64 icount = total / 16;
    if (icount < 256)
        icount = 256;
    if (icount > 65536)
        icount = 65536;
    icount = ALIGN_UP(icount, IPB);
    struct sb_disk sb;
    memset(&sb, 0, sizeof(sb));
    sb.version = 1;
    memcpy(sb.magic, "SONARFS1", 8);
    sb.block_size = BS;
    sb.inode_size = ISZ;
    sb.total_blocks = total;
    sb.inode_count = icount;
    sb.bitmap_start = 1;
    sb.bitmap_blocks = (total + BS * 8 - 1) / (BS * 8);
    sb.inode_start = sb.bitmap_start + sb.bitmap_blocks;
    sb.inode_blocks = icount / IPB;
    sb.data_start = sb.inode_start + sb.inode_blocks;
    if (sb.data_start + 8 > total) {
        snprintf(err, errlen, "%s is too small", d->name);
        return false;
    }
    sb.created = sb.mtime = now();
    strlcpy(sb.label, label && *label ? label : "SONAR", sizeof(sb.label));
    u64 seed = time_ns() ^ (sb.created << 20) ^ total;
    for (int i = 0; i < 16; i++) {
        seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
        sb.uuid[i] = (u8)(seed >> 56);
    }
    u8 *blk = kzalloc(BS);
    if (!blk) {
        snprintf(err, errlen, "out of memory");
        return false;
    }
    bool ok = true;
    /* zero the inode table */
    for (u64 b = sb.inode_start; ok && b < sb.data_start; b++)
        ok = blk_write(d, b * SPB, SPB, blk);
    /* bitmap: metadata blocks + root directory block used; bits past the end set */
    u64 used = sb.data_start + 1;
    for (u64 bb = 0; ok && bb < sb.bitmap_blocks; bb++) {
        memset(blk, 0, BS);
        for (u64 bit = 0; bit < BS * 8; bit++) {
            u64 b = bb * BS * 8 + bit;
            if (b < used || b >= total)
                blk[bit >> 3] |= (u8)(1u << (bit & 7));
        }
        ok = blk_write(d, (sb.bitmap_start + bb) * SPB, SPB, blk);
    }
    sb.free_blocks = total - used;
    sb.free_inodes = icount - 2; /* 0 is reserved, 1 is the root */
    /* root directory: "." and ".." */
    memset(blk, 0, BS);
    struct dirent_disk *de = (struct dirent_disk *)blk;
    de[0].ino = ROOT_INO;
    de[0].type = FT_DIR;
    de[0].name_len = 1;
    de[0].name[0] = '.';
    de[1].ino = ROOT_INO;
    de[1].type = FT_DIR;
    de[1].name_len = 2;
    de[1].name[0] = de[1].name[1] = '.';
    if (ok)
        ok = blk_write(d, sb.data_start * SPB, SPB, blk);
    struct inode_disk root;
    memset(&root, 0, sizeof(root));
    root.type = FT_DIR;
    root.links = 2;
    root.size = 2 * DE_SIZE;
    root.ctime = root.mtime = sb.created;
    root.nextents = 1;
    root.ext[0].start = sb.data_start;
    root.ext[0].len = 1;
    memset(blk, 0, BS);
    memcpy(blk + ROOT_INO * ISZ, &root, ISZ);
    if (ok)
        ok = blk_write(d, sb.inode_start * SPB, SPB, blk);
    memset(blk, 0, BS);
    memcpy(blk, &sb, sizeof(sb));
    if (ok)
        ok = blk_write(d, 0, SPB, blk) && blk_flush(d);
    kfree(blk);
    if (!ok)
        snprintf(err, errlen, "write error on %s", d->name);
    return ok;
}

int fs_mount(struct blkdev *d, bool ram)
{
    if (!fs_probe(d))
        return FS_ENOFS;
    u8 *blk = kmalloc(BS);
    if (!blk || !blk_read(d, 0, SPB, blk)) {
        kfree(blk);
        return FS_EIO;
    }
    struct sb_disk sb;
    memcpy(&sb, blk, sizeof(sb));
    if ((sb.flags & SB_JOURNAL) && !d->readonly && sb.journal_start &&
        sb.journal_start + JBLOCKS <= sb.total_blocks) {
        int n = journal_replay(d, &sb);
        if (n < 0) {
            kfree(blk);
            return FS_EIO;
        }
        if (n > 0) {
            printk("fs: %s: finished an interrupted update from the journal (%d sectors)\n",
                   d->name, n);
            if (!blk_read(d, 0, SPB, blk)) { /* the superblock may have changed */
                kfree(blk);
                return FS_EIO;
            }
            memcpy(&sb, blk, sizeof(sb));
        }
    }
    kfree(blk);
    if (sb.total_blocks > d->nsectors / SPB || sb.data_start >= sb.total_blocks ||
        sb.inode_start + sb.inode_blocks != sb.data_start || sb.inode_count != sb.inode_blocks * IPB)
        return FS_EINVAL;
    u8 *bm = kmalloc(sb.bitmap_blocks * BS);
    if (!bm)
        return FS_ENOMEM;
    for (u64 b = 0; b < sb.bitmap_blocks; b++) {
        if (!blk_read(d, (sb.bitmap_start + b) * SPB, SPB, bm + b * BS)) {
            kfree(bm);
            return FS_EIO;
        }
    }
    if (fs.mounted) {
        tx_commit();
        kfree(fs.bitmap);
    }
    fs.journal = false;
    fs.ntx = 0;
    memset(fds, 0, sizeof(fds));
    fs.dev = d;
    fs.sb = sb;
    fs.bitmap = bm;
    fs.dirty_lo = ~0ULL;
    fs.dirty_hi = 0;
    fs.inode_hint = 0;
    fs.ram = ram;
    fs.mounted = true;
    strlcpy(cwd, "/", sizeof(cwd));
    /* recount free blocks (cheap insurance against an unclean shutdown) */
    u64 nfree = 0;
    for (u64 b = 0; b < sb.total_blocks; b++)
        nfree += !bm_test(b);
    fs.sb.free_blocks = nfree;
    journal_start(d);
    return FS_OK;
}

bool fs_mounted(void) { return fs.mounted; }
bool fs_is_ram(void) { return fs.ram; }
const char *fs_device(void) { return fs.mounted ? fs.dev->name : "none"; }

void fs_usage(u64 *total, u64 *freeb, char *label, size_t n)
{
    *total = fs.mounted ? fs.sb.total_blocks * BS : 0;
    *freeb = fs.mounted ? fs.sb.free_blocks * BS : 0;
    if (label)
        strlcpy(label, fs.mounted ? fs.sb.label : "", n);
}
