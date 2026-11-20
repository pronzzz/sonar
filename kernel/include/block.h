/* Block devices: whole disks and their partitions. */
#pragma once
#include "types.h"

#define BLK_SECTOR 512

struct blkdev {
    char name[16];       /* vda, vda1, ... */
    u64 nsectors;        /* 512-byte sectors */
    bool readonly;
    struct blkdev *parent; /* whole disk, for partitions */
    u64 start;           /* first sector on the parent (partitions) */
    char label[40];      /* partition name (GPT) or "" */
    u8 type_guid[16];    /* GPT partition type, or MBR type in byte 0 */
    bool gpt;
    /* Disk drivers implement these (partitions forward to the parent). */
    bool (*read)(struct blkdev *d, u64 lba, u32 count, void *buf);
    bool (*write)(struct blkdev *d, u64 lba, u32 count, const void *buf);
    bool (*flush)(struct blkdev *d);
    void (*shutdown)(struct blkdev *d); /* before power-off (optional) */
    void *priv;
    void *mem;           /* contents directly addressable (RAM disks), else NULL */
};

void blk_register(struct blkdev *d); /* also scans its partition table */
int blk_count(void);
struct blkdev *blk_get(int i);
struct blkdev *blk_find(const char *name);

bool blk_read(struct blkdev *d, u64 lba, u32 count, void *buf);
bool blk_write(struct blkdev *d, u64 lba, u32 count, const void *buf);
bool blk_flush(struct blkdev *d);
/* Flush every disk and let drivers shut down cleanly (power-off, reboot). */
void blk_shutdown_all(void);
/* Byte-granular read (any offset/length); used for files on raw disks. */
bool blk_read_bytes(struct blkdev *d, u64 off, void *buf, u64 len);
void blk_format_size(u64 bytes, char *out, size_t n);
struct blkdev *ramdisk_create(const char *name, u64 bytes);
/* A block device over memory that already holds data (e.g. a file the UEFI
 * loader read); bytes is rounded up to whole sectors, which must be mapped. */
struct blkdev *ramdisk_attach(const char *name, void *mem, u64 bytes);
const char *blk_content_hint(struct blkdev *d);
