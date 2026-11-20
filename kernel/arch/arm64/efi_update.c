/*
 * UEFI loader: boot an installed update (docs/updates.md).
 *
 * The kernel on the EFI partition (\EFI\BOOT\BOOTAA64.EFI) is never
 * rewritten by the updater; it is the built-in kernel and the fallback.
 * `update install` writes a newer kernel, with its signed manifest, to
 * /boot/kernel on the SonarFS system disk. Here, before anything else,
 * the built-in kernel's loader:
 *
 *   1. finds a SonarFS volume through the firmware's Block I/O protocol
 *      and reads /boot/kernel with a small read-only SonarFS reader;
 *   2. checks the manifest's Ed25519 signature against the key compiled
 *      into this kernel, and the image's size and SHA-256 (bundle_verify);
 *   3. uses it only if it is newer than this kernel;
 *   4. counts attempts in a UEFI variable: an update that has not yet
 *      reached the network stage of boot (it then writes its hash to
 *      /boot/ok) gets UPDATE_MAX_TRIES starts, then is skipped;
 *   5. starts it with LoadImage/StartImage, telling it through its load
 *      options that it is the update (so it does not chain again).
 *
 * Any failure falls through to booting this kernel, which can always
 * repair or replace /boot/kernel (`update clear`, `update install`).
 *
 * Like efi.c this runs before relocation: no initialised pointers in data.
 */
#include "efi.h"
#include "crypto.h"
#include "efi_fw.h"
#include "kernel.h"
#include "string.h"
#include "update.h"

static const efi_guid BLOCK_IO_GUID = { 0x964e5b21, 0x6459, 0x11d2, { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } };
/* Sonar's own vendor GUID for its variables */
static const efi_guid SONAR_VAR_GUID = { 0x5a0e7a12, 0x50fa, 0x4f53, { 0x9d, 0x3e, 0x55, 0x50, 0x44, 0x41, 0x54, 0x45 } };

#define FS_BLOCK 4096
#define MAX_KERNEL (64ULL << 20)

static struct efi_boot_services *BS;
static struct efi_runtime_services *RT;
static void (*out)(const char *);

static void note(const char *fmt, ...) __printf(1, 2);
static void note(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(efi_handoff.update_note, sizeof(efi_handoff.update_note), fmt, ap);
    va_end(ap);
    out("  update: ");
    out(efi_handoff.update_note);
    out("\n");
}

static u32 le32(const u8 *p) { return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }
static u64 le64(const u8 *p) { return le32(p) | (u64)le32(p + 4) << 32; }

static void *pages(u64 bytes)
{
    u64 a;
    if (EFI_ERROR(BS->allocate_pages(0 /* any */, 2 /* EfiLoaderData */, (bytes + 4095) / 4096, &a)))
        return NULL;
    return (void *)(uintptr_t)a;
}
static void unpages(void *p, u64 bytes)
{
    if (p)
        BS->free_pages((u64)(uintptr_t)p, (bytes + 4095) / 4096);
}

/* ------------------------------------------------- read-only SonarFS */
struct vol {
    struct efi_block_io *bio;
    u64 total_blocks, inode_start, inode_count;
    u8 *blk; /* one block of scratch space */
};

static bool read_blocks(struct vol *v, u64 blk, u64 count, void *buf)
{
    if (blk + count > v->total_blocks || blk + count < blk)
        return false;
    u32 bs = v->bio->media->block_size;
    return !EFI_ERROR(v->bio->read_blocks(v->bio, v->bio->media->media_id, blk * (FS_BLOCK / bs),
                                          count * FS_BLOCK, buf));
}

struct ino {
    u16 type;
    u64 size;
    u32 nextents, ext_block;
    u8 inline_ext[80];
};

static bool read_inode(struct vol *v, u32 n, struct ino *ip)
{
    if (!n || n >= v->inode_count) /* inode n is at byte n * 128 of the table */
        return false;
    if (!read_blocks(v, v->inode_start + n / 32, 1, v->blk))
        return false;
    const u8 *d = v->blk + (n % 32) * 128;
    ip->type = (u16)(d[0] | d[1] << 8);
    ip->size = le64(d + 8);
    ip->nextents = le32(d + 32);
    ip->ext_block = le32(d + 36);
    memcpy(ip->inline_ext, d + 40, 80);
    return ip->type == 1 || ip->type == 2;
}

/* Read the first `cap` bytes of a file's data into buf (a multiple of 4096 bytes). */
static bool read_data(struct vol *v, const struct ino *ip, u8 *buf, u64 cap)
{
    u64 off = 0;
    u32 left = ip->nextents;
    const u8 *ext = ip->inline_ext;
    u32 in_block = left < 5 ? left : 5;
    u64 next = ip->ext_block;
    u8 *over = NULL;
    while (left && off < cap) {
        for (u32 i = 0; i < in_block && left && off < cap; i++, left--) {
            u64 start = le64(ext + 16 * i);
            u64 len = le32(ext + 16 * i + 8);
            if (len > (cap - off) / FS_BLOCK)
                len = (cap - off) / FS_BLOCK;
            if (!read_blocks(v, start, len, buf + off))
                goto fail;
            off += len * FS_BLOCK;
        }
        if (!left || off >= cap)
            break;
        /* overflow block: next (u64), count (u32), then up to 255 extents */
        if (!next)
            goto fail;
        if (!over && !(over = pages(FS_BLOCK)))
            goto fail;
        if (!read_blocks(v, next, 1, over))
            goto fail;
        next = le64(over);
        in_block = le32(over + 8);
        if (in_block > 255)
            goto fail;
        ext = over + 16;
    }
    unpages(over, FS_BLOCK);
    return off >= cap || off >= ip->size;
fail:
    unpages(over, FS_BLOCK);
    return false;
}

/* Inode number of NAME in directory DIR (0 if absent). */
static u32 dir_find(struct vol *v, u32 dir, const char *name, u16 want_type)
{
    struct ino d;
    if (!read_inode(v, dir, &d) || d.type != 2 || d.size > (1 << 20))
        return 0;
    u64 cap = (d.size + FS_BLOCK - 1) & ~(u64)(FS_BLOCK - 1);
    if (!cap)
        return 0;
    u8 *buf = pages(cap);
    u32 found = 0;
    size_t nl = strlen(name);
    if (buf && read_data(v, &d, buf, cap)) {
        for (u64 o = 0; o + 64 <= d.size; o += 64) {
            const u8 *e = buf + o;
            if (le32(e) && e[4] == want_type && e[5] == nl && !memcmp(e + 6, name, nl)) {
                found = le32(e);
                break;
            }
        }
    }
    unpages(buf, cap);
    return found;
}

/* Read a whole file (path components "boot", NAME); returns a page buffer. */
static u8 *read_boot_file(struct vol *v, const char *name, u64 max, u64 *size)
{
    u32 boot = dir_find(v, 1, "boot", 2);
    u32 n = boot ? dir_find(v, boot, name, 1) : 0;
    struct ino f;
    if (!n || !read_inode(v, n, &f) || f.type != 1 || !f.size || f.size > max)
        return NULL;
    u64 cap = (f.size + FS_BLOCK - 1) & ~(u64)(FS_BLOCK - 1);
    u8 *buf = pages(cap);
    if (!buf)
        return NULL;
    if (!read_data(v, &f, buf, cap)) {
        unpages(buf, cap);
        return NULL;
    }
    *size = f.size;
    return buf;
}

/* ------------------------------------------------------------- attempts */
struct tries {
    u8 sha[32];
    u32 count;
};

static u32 attempts(const u8 sha[32])
{
    struct tries t;
    u64 n = sizeof(t);
    u32 attr;
    if (!RT || EFI_ERROR(RT->get_variable(u"SonarUpdateTries", &SONAR_VAR_GUID, &attr, &n, &t)) ||
        n != sizeof(t) || memcmp(t.sha, sha, 32))
        return 0;
    return t.count;
}

static bool set_attempts(const u8 sha[32], u32 count)
{
    struct tries t;
    memcpy(t.sha, sha, 32);
    t.count = count;
    return RT && !EFI_ERROR(RT->set_variable(u"SonarUpdateTries", &SONAR_VAR_GUID,
                                             EFI_VARIABLE_NON_VOLATILE |
                                                 EFI_VARIABLE_BOOTSERVICE_ACCESS |
                                                 EFI_VARIABLE_RUNTIME_ACCESS,
                                             sizeof(t), &t));
}

/* --------------------------------------------------------------- boot */
static u16 chain_options[80]; /* L"sonar.chained <sha256 hex>" */
#define CHAIN_PREFIX "sonar.chained "

static bool is_chained(const struct efi_loaded_image *li)
{
    if (!li || !li->load_options || li->load_options_size < 2 * (sizeof(CHAIN_PREFIX) - 1 + 64))
        return false;
    const u16 *o = li->load_options;
    for (size_t i = 0; i < sizeof(CHAIN_PREFIX) - 1; i++)
        if (o[i] != (u16)CHAIN_PREFIX[i])
            return false;
    for (size_t i = 0; i < 64; i++) {
        u16 c = o[sizeof(CHAIN_PREFIX) - 1 + i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
            return false;
        efi_handoff.chained_sha[i] = (char)c;
    }
    efi_handoff.chained_sha[64] = 0;
    return true;
}

/* Returns only if it did not start the update: true if the volume has
 * /boot/kernel (it decided), false to look at the next volume. */
static bool try_volume(efi_handle image, struct efi_loaded_image *li, struct vol *v,
                       const u8 *sb)
{
    /* A committed journal transaction not yet applied means the metadata
     * on disk is not current; the kernel replays it at mount. */
    if (le32(sb + 4) & 1) {
        u64 js = le64(sb + 160);
        if (js && read_blocks(v, js, 1, v->blk) && !memcmp(v->blk, "SNRJRNL1", 8)) {
            note("the system disk has an unfinished journal transaction; using the built-in kernel");
            return true;
        }
    }
    u64 size = 0;
    u8 *bundle = read_boot_file(v, "kernel", MAX_KERNEL, &size);
    if (!bundle)
        return false; /* no update installed here */
    u64 cap = size;

    struct manifest m;
    const void *img;
    size_t img_len;
    int r = bundle_verify(bundle, size, &m, &img, &img_len);
    if (r != MF_OK) {
        note("ignoring /boot/kernel: %s", manifest_strerror(r));
        goto out;
    }
    if (version_compare(m.version, OS_VERSION) <= 0) {
        note("built-in kernel %s is as new as installed update %s; using the built-in one",
             OS_VERSION, m.version);
        goto out;
    }

    char sha[65];
    hex_encode(m.sha256, 32, sha);
    u64 oksize = 0;
    u8 *ok = read_boot_file(v, "ok", 4096, &oksize);
    bool confirmed = ok && oksize >= 64 && !memcmp(ok, sha, 64);
    unpages(ok, oksize);
    if (!confirmed) {
        u32 n = attempts(m.sha256);
        if (n >= UPDATE_MAX_TRIES) {
            note("update %s did not finish starting %u times; using the built-in kernel %s "
                 "('update clear' removes it)", m.version, n, OS_VERSION);
            goto out;
        }
        if (!set_attempts(m.sha256, n + 1))
            out("  update: (this firmware cannot count attempts in a UEFI variable)\n");
    }

    char line[120];
    snprintf(line, sizeof(line), "  update: starting Sonar %s from /boot/kernel (signature %s)%s\n",
             m.version, manifest_strerror(MF_OK), confirmed ? "" : ", trial start");
    out(line);

    efi_handle child;
    efi_status s = BS->load_image(false, image, NULL, (void *)img, img_len, &child);
    if (EFI_ERROR(s)) {
        note("the firmware could not load update %s (status %#llx); using the built-in kernel",
             m.version, (unsigned long long)s);
        goto out;
    }
    struct efi_loaded_image *cli;
    if (!EFI_ERROR(BS->handle_protocol(child, &LOADED_IMAGE_GUID, (void **)&cli))) {
        /* the update reads board.dtb, system.img and the model from our drive */
        cli->device_handle = li->device_handle;
        cli->file_path = li->file_path;
        size_t n = 0;
        for (const char *p = CHAIN_PREFIX; *p; p++)
            chain_options[n++] = (u16)*p;
        for (int i = 0; i < 64; i++)
            chain_options[n++] = (u16)sha[i];
        chain_options[n++] = 0;
        cli->load_options = chain_options;
        cli->load_options_size = (u32)(n * 2);
    }
    s = BS->start_image(child, NULL, NULL);
    /* Only reached if the update returned to the firmware instead of booting. */
    BS->unload_image(child);
    note("update %s stopped before starting (status %#llx); using the built-in kernel",
         m.version, (unsigned long long)s);
out:
    unpages(bundle, cap);
    return true;
}

void efi_boot_update(efi_handle image, struct efi_system_table *st, struct efi_loaded_image *li,
                     void (*say)(const char *))
{
    BS = st->boot_services;
    RT = st->runtime_services;
    out = say;
    if (is_chained(li)) {
        efi_handoff.chained = 1;
        say("  update: this is the installed update, started by the built-in kernel\n");
        return;
    }

    u64 count = 0;
    efi_handle *handles = NULL;
    if (EFI_ERROR(BS->locate_handle_buffer(EFI_BY_PROTOCOL, &BLOCK_IO_GUID, NULL, &count, &handles)))
        return;
    u8 *sb = pages(FS_BLOCK), *scratch = pages(FS_BLOCK);
    for (u64 i = 0; sb && scratch && i < count; i++) {
        struct efi_block_io *bio;
        if (EFI_ERROR(BS->handle_protocol(handles[i], &BLOCK_IO_GUID, (void **)&bio)) || !bio->media ||
            !bio->media->present)
            continue;
        u32 bs = bio->media->block_size;
        if (bs < 512 || bs > FS_BLOCK || (bs & (bs - 1)) || bio->media->io_align > FS_BLOCK)
            continue;
        if (EFI_ERROR(bio->read_blocks(bio, bio->media->media_id, 0, FS_BLOCK, sb)))
            continue;
        if (memcmp(sb + 8, "SONARFS1", 8) || le32(sb) != 1 || le32(sb + 16) != FS_BLOCK ||
            le32(sb + 20) != 128)
            continue;
        struct vol v = { bio, le64(sb + 24), le64(sb + 56), le64(sb + 32), scratch };
        u64 disk_blocks = (bio->media->last_block + 1) / (FS_BLOCK / bs);
        if (v.total_blocks > disk_blocks)
            continue;
        if (try_volume(image, li, &v, sb))
            break; /* the first volume with /boot/kernel decides */
    }
    unpages(sb, FS_BLOCK);
    unpages(scratch, FS_BLOCK);
    BS->free_pool(handles);
}
