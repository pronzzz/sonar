/* RAM disk: a block device in memory (root file system fallback). */
#include "block.h"
#include "kernel.h"
#include "mm.h"
#include "string.h"

static bool rd_read(struct blkdev *d, u64 lba, u32 count, void *buf)
{
    memcpy(buf, (u8 *)d->priv + lba * BLK_SECTOR, (size_t)count * BLK_SECTOR);
    return true;
}

static bool rd_write(struct blkdev *d, u64 lba, u32 count, const void *buf)
{
    memcpy((u8 *)d->priv + lba * BLK_SECTOR, buf, (size_t)count * BLK_SECTOR);
    return true;
}

struct blkdev *ramdisk_create(const char *name, u64 bytes)
{
    bytes = ALIGN_UP(bytes, PAGE_SIZE);
    void *mem = page_alloc(bytes / PAGE_SIZE);
    struct blkdev *d = kzalloc(sizeof(*d));
    if (!mem || !d) {
        if (mem)
            page_free(mem, bytes / PAGE_SIZE);
        kfree(d);
        return NULL;
    }
    strlcpy(d->name, name, sizeof(d->name));
    d->nsectors = bytes / BLK_SECTOR;
    d->read = rd_read;
    d->write = rd_write;
    d->priv = mem;
    d->mem = mem;
    blk_register(d);
    return d;
}

struct blkdev *ramdisk_attach(const char *name, void *mem, u64 bytes)
{
    struct blkdev *d = kzalloc(sizeof(*d));
    if (!d)
        return NULL;
    strlcpy(d->name, name, sizeof(d->name));
    d->nsectors = (bytes + BLK_SECTOR - 1) / BLK_SECTOR;
    d->read = rd_read;
    d->write = rd_write;
    d->priv = mem;
    d->mem = mem;
    blk_register(d);
    return d;
}
