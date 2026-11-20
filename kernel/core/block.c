/*
 * Block device registry, partition tables (GPT and MBR), and the `disks`
 * shell command.
 */
#include "block.h"
#include "irq.h"
#include "kernel.h"
#include "mm.h"
#include "shell.h"
#include "string.h"

#define MAX_BLK 32
static struct blkdev *devs[MAX_BLK];
static int ndevs;

int blk_count(void) { return ndevs; }
struct blkdev *blk_get(int i) { return i >= 0 && i < ndevs ? devs[i] : NULL; }

struct blkdev *blk_find(const char *name)
{
    if (!strncmp(name, "/dev/", 5))
        name += 5;
    for (int i = 0; i < ndevs; i++)
        if (!strcmp(devs[i]->name, name))
            return devs[i];
    return NULL;
}

bool blk_read(struct blkdev *d, u64 lba, u32 count, void *buf)
{
    if (!count)
        return true;
    if (lba >= d->nsectors || count > d->nsectors - lba)
        return false;
    if (d->parent)
        return blk_read(d->parent, d->start + lba, count, buf);
    return d->read(d, lba, count, buf);
}

bool blk_write(struct blkdev *d, u64 lba, u32 count, const void *buf)
{
    if (!count)
        return true;
    if (d->readonly || lba >= d->nsectors || count > d->nsectors - lba)
        return false;
    if (d->parent)
        return blk_write(d->parent, d->start + lba, count, buf);
    return d->write && d->write(d, lba, count, buf);
}

bool blk_flush(struct blkdev *d)
{
    while (d->parent)
        d = d->parent;
    return !d->flush || d->flush(d);
}

void blk_shutdown_all(void)
{
    for (int i = 0; i < blk_count(); i++) {
        struct blkdev *d = blk_get(i);
        if (d->parent)
            continue;
        if (d->shutdown)
            d->shutdown(d);
        else if (d->flush)
            d->flush(d);
    }
}

bool blk_read_bytes(struct blkdev *d, u64 off, void *buf, u64 len)
{
    u8 *out = buf;
    u8 sec[BLK_SECTOR] __attribute__((aligned(16)));
    if (off + len < off || off + len > d->nsectors * BLK_SECTOR)
        return false;
    while (len) {
        u64 lba = off / BLK_SECTOR;
        u32 in = (u32)(off % BLK_SECTOR);
        if (in || len < BLK_SECTOR) {
            /* partial sector through a bounce buffer */
            if (!blk_read(d, lba, 1, sec))
                return false;
            u64 n = MIN((u64)(BLK_SECTOR - in), len);
            memcpy(out, sec + in, n);
            out += n;
            off += n;
            len -= n;
        } else {
            /* whole sectors straight into the destination */
            u64 nsec = len / BLK_SECTOR;
            while (nsec) {
                u32 c = (u32)MIN(nsec, (u64)1 << 20);
                if (!blk_read(d, lba, c, out))
                    return false;
                out += (u64)c * BLK_SECTOR;
                off += (u64)c * BLK_SECTOR;
                len -= (u64)c * BLK_SECTOR;
                lba += c;
                nsec -= c;
            }
        }
    }
    return true;
}

void blk_format_size(u64 bytes, char *out, size_t n)
{
    const char *unit[] = { "B", "KiB", "MiB", "GiB", "TiB" };
    int u = 0;
    u64 whole = bytes, frac = 0;
    while (whole >= 1024 && u < 4) {
        frac = whole % 1024;
        whole /= 1024;
        u++;
    }
    if (u == 0)
        snprintf(out, n, "%llu B", (unsigned long long)bytes);
    else
        snprintf(out, n, "%llu.%llu %s", (unsigned long long)whole,
                 (unsigned long long)(frac * 10 / 1024), unit[u]);
}

static void add_partition(struct blkdev *disk, int num, u64 start, u64 count, const char *label,
                          const u8 *type, bool gpt)
{
    if (ndevs >= MAX_BLK || !count || start >= disk->nsectors || count > disk->nsectors - start)
        return;
    struct blkdev *p = kzalloc(sizeof(*p));
    size_t dl = strlen(disk->name);
    bool digit = dl && disk->name[dl - 1] >= '0' && disk->name[dl - 1] <= '9';
    snprintf(p->name, sizeof(p->name), digit ? "%sp%d" : "%s%d", disk->name, num);
    if (disk->mem)
        p->mem = (u8 *)disk->mem + start * BLK_SECTOR;
    p->nsectors = count;
    p->readonly = disk->readonly;
    p->parent = disk;
    p->start = start;
    p->gpt = gpt;
    if (label)
        strlcpy(p->label, label, sizeof(p->label));
    memcpy(p->type_guid, type, gpt ? 16 : 1);
    devs[ndevs++] = p;
}

static bool scan_gpt(struct blkdev *d, u8 *buf)
{
    if (!blk_read(d, 1, 1, buf) || memcmp(buf, "EFI PART", 8))
        return false;
    u64 entries_lba = *(u64 *)(buf + 72);
    u32 n = *(u32 *)(buf + 80), esz = *(u32 *)(buf + 84);
    if (esz < 128 || esz > BLK_SECTOR || BLK_SECTOR % esz || n > 1024)
        return false;
    u32 per = BLK_SECTOR / esz;
    int num = 0;
    for (u32 i = 0; i < n; i++) {
        if (i % per == 0 && !blk_read(d, entries_lba + i / per, 1, buf))
            break;
        const u8 *e = buf + (i % per) * esz;
        static const u8 zero[16];
        num++;
        if (!memcmp(e, zero, 16))
            continue;
        u64 first = *(const u64 *)(e + 32), last = *(const u64 *)(e + 40);
        char label[40];
        int k = 0;
        for (int c = 0; c < 36 && k < 39; c++) {
            u16 ch = (u16)(e[56 + 2 * c] | e[57 + 2 * c] << 8);
            if (!ch)
                break;
            label[k++] = ch < 128 ? (char)ch : '?';
        }
        label[k] = 0;
        if (last >= first)
            add_partition(d, num, first, last - first + 1, label, e, true);
    }
    return true;
}

static void scan_mbr(struct blkdev *d, u8 *buf)
{
    if (!blk_read(d, 0, 1, buf) || buf[510] != 0x55 || buf[511] != 0xaa)
        return;
    /* A GGUF file or filesystem on a raw disk is not a partition table. */
    if (!memcmp(buf, "GGUF", 4))
        return;
    for (int i = 0; i < 4; i++) {
        const u8 *e = buf + 446 + 16 * i;
        u8 type = e[4];
        u32 start = (u32)(e[8] | e[9] << 8 | e[10] << 16 | (u32)e[11] << 24);
        u32 count = (u32)(e[12] | e[13] << 8 | e[14] << 16 | (u32)e[15] << 24);
        if (!type || type == 0xee || type == 0x05 || type == 0x0f || type == 0x85)
            continue; /* empty, protective GPT, extended */
        add_partition(d, i + 1, start, count, NULL, &type, false);
    }
}

void blk_register(struct blkdev *d)
{
    if (ndevs >= MAX_BLK)
        return;
    devs[ndevs++] = d;
    u8 *buf = kmalloc(BLK_SECTOR);
    if (!scan_gpt(d, buf))
        scan_mbr(d, buf);
    kfree(buf);
}

/* What is on a device, judged from its first sector. */
const char *blk_content_hint(struct blkdev *d)
{
    static char hint[48];
    u8 buf[BLK_SECTOR];
    if (!blk_read(d, 0, 1, buf))
        return "unreadable";
    if (!memcmp(buf, "GGUF", 4))
        return "GGUF model";
    if (!memcmp(buf + 8, "SONARFS1", 8))
        return "SonarFS";
    if (!d->parent) {
        for (int i = 0; i < ndevs; i++)
            if (devs[i]->parent == d)
                return "partitioned";
    }
    if (d->parent && !d->gpt) {
        snprintf(hint, sizeof(hint), "MBR type %#02x", d->type_guid[0]);
        return hint;
    }
    return "-";
}

static int cmd_disks(int argc, char **argv)
{
    if (!ndevs) {
        printk("no block devices\n");
        return 0;
    }
    printk("%-8s %11s  %-3s %-12s %s\n", "NAME", "SIZE", "RO", "CONTENTS", "LABEL");
    for (int i = 0; i < ndevs; i++) {
        struct blkdev *d = devs[i];
        char sz[24];
        blk_format_size(d->nsectors * BLK_SECTOR, sz, sizeof(sz));
        printk("%-8s %11s  %-3s %-12s %s\n", d->name, sz, d->readonly ? "yes" : "no",
               blk_content_hint(d), d->label);
    }
    return 0;
}
SHELL_COMMAND(disks, "disks", "disks", "list disks and partitions", cmd_disks);

static int cmd_diskread(int argc, char **argv)
{
    if (argc < 2) {
        printk("usage: diskread DEV [MiB]\n");
        return 1;
    }
    struct blkdev *d = blk_find(argv[1]);
    if (!d) {
        printk("diskread: no such device: %s\n", argv[1]);
        return 1;
    }
    u64 mib = argc > 2 ? strtoull(argv[2], NULL, 10) : 256;
    u64 bytes = MIN(mib << 20, d->nsectors * BLK_SECTOR);
    const u32 chunk = 16u << 20;
    u8 *buf = page_alloc_nozero(chunk / PAGE_SIZE);
    if (!buf) {
        printk("diskread: out of memory\n");
        return 1;
    }
    u64 t0 = time_ns(), done = 0;
    while (done < bytes) {
        u32 n = (u32)MIN((u64)chunk, bytes - done);
        if (!blk_read(d, done / BLK_SECTOR, (n + BLK_SECTOR - 1) / BLK_SECTOR, buf)) {
            printk("diskread: I/O error at byte %llu\n", (unsigned long long)done);
            break;
        }
        done += n;
    }
    u64 ns = time_ns() - t0;
    page_free(buf, chunk / PAGE_SIZE);
    printk("read %llu MiB in %llu ms (%llu MiB/s)\n", (unsigned long long)(done >> 20),
           (unsigned long long)(ns / 1000000),
           (unsigned long long)(ns ? (done >> 20) * 1000000000ULL / ns : 0));
    return 0;
}
SHELL_COMMAND(diskread, "diskread", "diskread DEV [MiB]", "measure disk read speed", cmd_diskread);
