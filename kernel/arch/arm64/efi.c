/*
 * UEFI loader stub (UEFI 2.x specification; same approach as Linux's arm64
 * EFI stub). Firmware calls efi_entry (boot.S), which calls efi_main()
 * here with the MMU on, firmware page tables and the firmware's stack.
 *
 * efi_main() runs before the kernel has applied its own relocations, so
 * this file must not use initialised pointers in data (string literals,
 * GUIDs and other plain data are fine: they are reached PC-relative).
 *
 * It gathers:
 *   - the device tree: \EFI\sonar\board.dtb on the boot drive if present
 *     (for machines whose firmware only offers ACPI), else the firmware's;
 *   - the GOP framebuffer, for the console;
 *   - \sonar\system.img and \sonar\model.gguf from the boot drive, read
 *     into memory with the firmware's own disk drivers, so Sonar and
 *     DeepSeek run on machines Sonar has no storage driver for;
 *   - the memory map, at ExitBootServices.
 * then returns the device tree address; efi_entry turns the MMU off and
 * enters the kernel exactly as a Linux-style bootloader would.
 */
#include "efi.h"
#include "acpi_fdt.h"
#include "arch.h"
#include "kernel.h"
#include "string.h"

struct efi_handoff efi_handoff __attribute__((section(".data.efi_handoff")));

#include "efi_fw.h"

static struct efi_system_table *ST;
static struct efi_boot_services *BS;

/* ---------------------------------------------------------------- output */
/* Everything the loader prints is also kept here and saved to
 * \sonar\boot.log on the boot drive, since the firmware console may be a
 * screen nobody can capture (or no console at all). */
static char boot_log[8192];
static size_t boot_log_len;

static void say(const char *s)
{
    for (const char *p = s; *p && boot_log_len < sizeof(boot_log) - 1; p++)
        boot_log[boot_log_len++] = *p;
    if (!ST->con_out)
        return;
    u16 buf[128];
    size_t n = 0;
    for (; *s; s++) {
        if (*s == '\n')
            buf[n++] = '\r';
        buf[n++] = (u8)*s;
        if (n >= 120) {
            buf[n] = 0;
            ST->con_out->output_string(ST->con_out, buf);
            n = 0;
        }
    }
    buf[n] = 0;
    ST->con_out->output_string(ST->con_out, buf);
}

static void sayf(const char *fmt, ...)
{
    char buf[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    say(buf);
}

#define EFI_FILE_MODE_RWC 0x8000000000000003ULL /* read, write, create */

static void save_boot_log(struct efi_file *root)
{
    struct efi_file *dir, *f;
    if (!root || EFI_ERROR(root->open(root, &dir, u"\\sonar", 0x8000000000000003ULL, 0x10)))
        return;
    dir->close(dir);
    /* Delete any old log first: a shorter write would leave its tail. */
    if (!EFI_ERROR(root->open(root, &f, u"\\sonar\\boot.log", EFI_FILE_MODE_RWC, 0)))
        f->delete_(f);
    if (EFI_ERROR(root->open(root, &f, u"\\sonar\\boot.log", EFI_FILE_MODE_RWC, 0)))
        return;
    u64 n = boot_log_len;
    f->write(f, &n, boot_log);
    f->flush(f);
    f->close(f);
}

static void *acpi_alloc(u64 size)
{
    u64 addr;
    if (EFI_ERROR(BS->allocate_pages(0 /* any */, 2 /* EfiLoaderData */, (size + 4095) / 4096, &addr)))
        return NULL;
    return (void *)(uintptr_t)addr;
}

static bool guid_eq(const efi_guid *a, const efi_guid *b) { return memcmp(a, b, sizeof(*a)) == 0; }

static void *config_table(const efi_guid *g)
{
    for (u64 i = 0; i < ST->number_of_table_entries; i++)
        if (guid_eq(&ST->configuration_table[i].guid, g))
            return ST->configuration_table[i].table;
    return NULL;
}

static u32 be32(const void *p)
{
    const u8 *b = p;
    return (u32)b[0] << 24 | (u32)b[1] << 16 | (u32)b[2] << 8 | b[3];
}

static bool valid_fdt(const void *p, u64 avail)
{
    return p && be32(p) == 0xd00dfeed && be32((const u8 *)p + 4) >= 40 &&
           be32((const u8 *)p + 4) <= avail;
}

/* ------------------------------------------------------------ boot drive */
static struct efi_file *boot_root(efi_handle image)
{
    struct efi_loaded_image *li;
    struct efi_simple_fs *fs;
    struct efi_file *root;
    if (EFI_ERROR(BS->handle_protocol(image, &LOADED_IMAGE_GUID, (void **)&li)) ||
        EFI_ERROR(BS->handle_protocol(li->device_handle, &SIMPLE_FS_GUID, (void **)&fs)) ||
        EFI_ERROR(fs->open_volume(fs, &root)))
        return NULL;
    return root;
}

/*
 * Read a file into fresh pages (EfiLoaderData); returns its address, or 0
 * if it does not exist or cannot be read. FAT32 limits files to 4 GiB, so
 * a larger file may be split into parts: PATH, PATH.1, PATH.2, ... are read
 * one after another into one contiguous buffer.
 */
#define MAX_PARTS 16
static u64 load_file(struct efi_file *root, const u16 *path, const char *label, u64 *size_out)
{
    if (!root)
        return 0;
    struct efi_file *parts[MAX_PARTS];
    u64 sizes[MAX_PARTS], total = 0;
    int np = 0;
    for (int i = 0; i < MAX_PARTS; i++) {
        u16 name[80];
        size_t n = 0;
        while (path[n] && n < 72) {
            name[n] = path[n];
            n++;
        }
        if (i > 0) {
            name[n++] = '.';
            if (i >= 10)
                name[n++] = (u16)('0' + i / 10);
            name[n++] = (u16)('0' + i % 10);
        }
        name[n] = 0;
        struct efi_file *f;
        u64 size = 0;
        if (EFI_ERROR(root->open(root, &f, name, EFI_FILE_MODE_READ, 0)))
            break;
        if (EFI_ERROR(f->set_position(f, ~0ULL)) || EFI_ERROR(f->get_position(f, &size)) ||
            EFI_ERROR(f->set_position(f, 0)) || !size) {
            f->close(f);
            break;
        }
        parts[np] = f;
        sizes[np] = size;
        total += size;
        np++;
    }
    if (!np)
        return 0;

    u64 addr = 0, pages = (total + 4095) / 4096;
    if (EFI_ERROR(BS->allocate_pages(AllocateAnyPages, EfiLoaderData, pages, &addr))) {
        sayf("  %s: %llu MiB does not fit in memory; skipped\n", label,
             (unsigned long long)(total >> 20));
        addr = 0;
        goto out;
    }
    /* Large reads in pieces, with progress: a model can be many GiB. */
    const u64 chunk = 32ULL << 20;
    u64 done = 0;
    int shown = -1;
    for (int p = 0; p < np; p++) {
        for (u64 off = 0; off < sizes[p];) {
            u64 n = sizes[p] - off < chunk ? sizes[p] - off : chunk;
            u64 got = n;
            if (EFI_ERROR(parts[p]->read(parts[p], &got, (void *)(uintptr_t)(addr + done))) ||
                got != n) {
                sayf("\n  %s: read error at %llu MiB; skipped\n", label,
                     (unsigned long long)(done >> 20));
                BS->free_pages(addr, pages);
                addr = 0;
                goto out;
            }
            off += n;
            done += n;
            int pct = (int)(done * 100 / total);
            if (total > chunk && pct / 5 != shown) {
                shown = pct / 5;
                sayf("\r  loading %s: %3d%% of %llu MiB", label, pct,
                     (unsigned long long)(total >> 20));
            }
        }
    }
    if (total > chunk)
        say(np > 1 ? " (split file)\n" : "\n");
    else
        sayf("  loaded %s (%llu KiB)\n", label, (unsigned long long)((total + 1023) >> 10));
    *size_out = total;
out:
    for (int p = 0; p < np; p++)
        parts[p]->close(parts[p]);
    return addr;
}

static void add_file(const char *name, const char *file, u64 base, u64 size)
{
    if (!base || efi_handoff.nfiles >= EFI_MAX_FILES)
        return;
    struct efi_file_image *fi = &efi_handoff.files[efi_handoff.nfiles++];
    fi->base = base;
    fi->size = size;
    strlcpy(fi->name, name, sizeof(fi->name));
    strlcpy(fi->file, file, sizeof(fi->file));
}

/* ----------------------------------------------------------------- video */
static void find_framebuffer(void)
{
    struct efi_gop *gop;
    if (EFI_ERROR(BS->locate_protocol(&GOP_GUID, NULL, (void **)&gop)) || !gop->mode ||
        !gop->mode->info) {
        say("  no graphics output\n");
        return;
    }
    struct efi_gop_mode_info *mi = gop->mode->info;
    u32 bgr;
    switch (mi->pixel_format) {
    case 0: bgr = 1; break; /* PixelRedGreenBlueReserved8BitPerColor: bytes R,G,B */
    case 1: bgr = 0; break; /* PixelBlueGreenRedReserved8BitPerColor: bytes B,G,R */
    case 2:                 /* PixelBitMask */
        if (mi->red_mask == 0xff0000 && mi->green_mask == 0xff00 && mi->blue_mask == 0xff)
            bgr = 0;
        else if (mi->red_mask == 0xff && mi->green_mask == 0xff00 && mi->blue_mask == 0xff0000)
            bgr = 1;
        else {
            say("  graphics output uses an unsupported pixel layout\n");
            return;
        }
        break;
    default:
        say("  graphics output has no linear framebuffer\n");
        return;
    }
    if (!gop->mode->frame_buffer_base || mi->width < 320 || mi->height < 200 ||
        (u64)mi->pixels_per_scan_line * 4 * mi->height > gop->mode->frame_buffer_size + 4096) {
        say("  graphics output framebuffer is unusable\n");
        return;
    }
    efi_handoff.fb_base = gop->mode->frame_buffer_base;
    efi_handoff.fb_size = gop->mode->frame_buffer_size;
    efi_handoff.fb_width = mi->width;
    efi_handoff.fb_height = mi->height;
    efi_handoff.fb_stride = mi->pixels_per_scan_line * 4;
    efi_handoff.fb_bgr = bgr;
    sayf("  display: %ux%u at %#llx\n", mi->width, mi->height,
         (unsigned long long)efi_handoff.fb_base);
}

/* ---------------------------------------------------------------- memory */
static void add_range(u64 base, u64 size)
{
    struct efi_handoff *h = &efi_handoff;
    if (h->nranges && h->ranges[h->nranges - 1].base + h->ranges[h->nranges - 1].size == base) {
        h->ranges[h->nranges - 1].size += size;
        return;
    }
    if (h->nranges == EFI_MAX_RANGES) {
        h->dropped_ranges++;
        return;
    }
    h->ranges[h->nranges].base = base;
    h->ranges[h->nranges].size = size;
    h->nranges++;
}

/* Runs after ExitBootServices: pure computation, no firmware calls. */
static void digest_memory_map(const u8 *map, u64 size, u64 desc_size)
{
    for (u64 off = 0; off + desc_size <= size; off += desc_size) {
        const struct efi_memory_descriptor *d = (const void *)(map + off);
        u64 bytes = d->pages * 4096;
        if (efi_handoff.fb_base >= d->phys && efi_handoff.fb_base < d->phys + bytes &&
            (d->attr & EFI_MEMORY_WB))
            efi_handoff.fb_in_ram = 1;
        if (!(d->attr & EFI_MEMORY_WB))
            continue;
        switch (d->type) {
        case EfiLoaderCode:
        case EfiLoaderData:          /* includes this image and the loaded files (reserved later) */
        case EfiBootServicesCode:
        case EfiBootServicesData:
        case EfiConventionalMemory:
            add_range(d->phys, bytes);
            break;
        default:                     /* firmware runtime, ACPI, reserved: keep out */
            break;
        }
    }
}

static void ascii_vendor(const u16 *v)
{
    size_t n = 0;
    if (v)
        for (; v[n] && n < sizeof(efi_handoff.firmware) - 1; n++)
            efi_handoff.firmware[n] = v[n] < 128 ? (char)v[n] : '?';
    efi_handoff.firmware[n] = 0;
}

/* ------------------------------------------------------------------ time */
static u64 days_from_civil(int y, unsigned m, unsigned d) /* H. Hinnant's algorithm */
{
    y -= m <= 2;
    int era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (u64)(era * 146097 + (int)doe - 719468);
}

static void read_time(void)
{
    struct efi_runtime_services *rt = ST->runtime_services;
    struct efi_time t;
    if (!rt || EFI_ERROR(rt->get_time(&t, NULL)) || t.year < 2000 || t.month < 1 || t.month > 12)
        return;
    u64 epoch = days_from_civil(t.year, t.month, t.day) * 86400 + t.hour * 3600u + t.minute * 60u + t.second;
    if (t.timezone >= -1440 && t.timezone <= 1440) /* minutes from UTC; 2047 = unspecified */
        epoch -= (i64)t.timezone * 60;
    efi_handoff.boot_epoch = epoch;
    efi_handoff.boot_counter = read_sysreg(cntvct_el0);
    sayf("  clock: %04u-%02u-%02u %02u:%02u:%02u\n", t.year, t.month, t.day, t.hour, t.minute, t.second);
}

/* ------------------------------------------------------------------ main */
u64 efi_main(efi_handle image, struct efi_system_table *st)
{
    ST = st;
    BS = st->boot_services;
    memset(&efi_handoff, 0, sizeof(efi_handoff));
    ascii_vendor(st->firmware_vendor);
    efi_handoff.firmware_rev = st->firmware_revision;
    efi_handoff.uefi_rev = st->hdr.revision;
    sayf("\nSonar %s UEFI loader (firmware: %s, UEFI %u.%u)\n", OS_VERSION, efi_handoff.firmware,
         st->hdr.revision >> 16, (st->hdr.revision & 0xffff) / 10);

    /* A newer, signed kernel installed on the system disk (docs/updates.md)
     * starts here instead of this one; this returns if there is none, or
     * if this kernel is that update. */
    struct efi_loaded_image *li = NULL;
    BS->handle_protocol(image, &LOADED_IMAGE_GUID, (void **)&li);
    efi_boot_update(image, st, li, say);

    struct efi_file *root = boot_root(image);

    /* Device tree: a file on the boot drive overrides the firmware's. */
    u64 dtb_size = 0;
    u64 dtb = load_file(root, u"\\EFI\\sonar\\board.dtb", "board.dtb", &dtb_size);
    if (dtb && valid_fdt((void *)(uintptr_t)dtb, dtb_size)) {
        efi_handoff.dtb_from_file = 1;
    } else {
        if (dtb)
            say("  \\EFI\\sonar\\board.dtb is not a device tree blob; ignored\n");
        void *fw = config_table(&DTB_GUID);
        dtb = valid_fdt(fw, 64 << 20) ? (u64)(uintptr_t)fw : 0;
    }
    efi_handoff.acpi_rsdp = (u64)(uintptr_t)config_table(&ACPI20_GUID);
    if (!dtb && efi_handoff.acpi_rsdp) {
        say("  no device tree from the firmware; building one from its ACPI tables\n");
        dtb = acpi_build_fdt(efi_handoff.acpi_rsdp, acpi_alloc, say);
        efi_handoff.dtb_from_acpi = dtb != 0;
    }
    if (!dtb) {
        say("\nSonar needs a device tree, and this firmware offers none");
        say(efi_handoff.acpi_rsdp ? " (it describes the machine with ACPI only).\n" : ".\n");
        say("Put the machine's .dtb file at \\EFI\\sonar\\board.dtb on the boot drive.\n");
        save_boot_log(root);
        if (root)
            root->close(root);
        return 0;
    }
    sayf("  device tree: %s\n", efi_handoff.dtb_from_file ? "\\EFI\\sonar\\board.dtb"
                               : efi_handoff.dtb_from_acpi ? "built from ACPI" : "from firmware");

    find_framebuffer();

    /* The system image and the model, read with the firmware's drivers. */
    u64 sz = 0, a = load_file(root, u"\\sonar\\system.img", "system.img", &sz);
    add_file("boot0", "system.img", a, sz);
    sz = 0;
    a = load_file(root, u"\\sonar\\model.gguf", "model.gguf", &sz);
    add_file("boot1", "model.gguf", a, sz);
    say("  starting the kernel\n");
    save_boot_log(root);
    if (root)
        root->close(root);

    read_time();

    /* Memory map, then ExitBootServices. After a failed ExitBootServices
     * only GetMemoryMap may be called, so the buffer is allocated once with
     * room to spare. */
    u64 map_size = 0, key, desc_size = 0;
    u32 desc_ver;
    BS->get_memory_map(&map_size, NULL, &key, &desc_size, &desc_ver);
    if (!desc_size)
        desc_size = sizeof(struct efi_memory_descriptor);
    u64 cap = map_size + 32 * desc_size;
    void *map;
    if (EFI_ERROR(BS->allocate_pool(EfiLoaderData, cap, &map))) {
        say("Sonar: out of memory for the memory map\n");
        return 0;
    }
    for (int tries = 0;; tries++) {
        map_size = cap;
        efi_status s = BS->get_memory_map(&map_size, map, &key, &desc_size, &desc_ver);
        if (!EFI_ERROR(s) && !EFI_ERROR(BS->exit_boot_services(image, key)))
            break;
        if (tries == 8) {
            say("Sonar: could not leave the firmware (ExitBootServices failed)\n");
            return 0;
        }
    }
    /* Boot services are gone: no firmware calls (or output) from here on. */
    ST = NULL;
    BS = NULL;
    digest_memory_map(map, map_size, desc_size);
    efi_handoff.magic = EFI_HANDOFF_MAGIC;
    return dtb;
}
