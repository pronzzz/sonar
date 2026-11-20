/*
 * UEFI boot. The kernel image is also a PE/COFF EFI application (header in
 * boot.S); when firmware starts it, efi_main() (kernel/arch/arm64/efi.c)
 * collects what the kernel needs from the firmware, exits boot services and
 * enters the kernel through the normal arm64 boot path. What it found is
 * passed in efi_handoff, which lives in .data so the kernel's .bss clearing
 * keeps it.
 */
#pragma once
#include "types.h"

#define EFI_HANDOFF_MAGIC 0x52414e4f53494645ULL /* "EFISONAR" */
#define EFI_MAX_RANGES 128
#define EFI_MAX_FILES 4

struct efi_range {
    u64 base, size;
};

/* A file the loader read into memory from the boot drive. */
struct efi_file_image {
    u64 base, size; /* page-aligned base; size in bytes */
    char name[16];  /* block device name the kernel gives it: boot0, boot1 */
    char file[24];  /* file name on the boot drive */
};

struct efi_handoff {
    u64 magic;              /* EFI_HANDOFF_MAGIC when booted through UEFI */
    u32 nranges;            /* usable RAM after ExitBootServices */
    u32 dropped_ranges;     /* ranges beyond EFI_MAX_RANGES (lost) */
    struct efi_range ranges[EFI_MAX_RANGES];
    u64 fb_base, fb_size;   /* GOP framebuffer, 0 if none */
    u32 fb_width, fb_height, fb_stride; /* stride in bytes */
    u32 fb_bgr;             /* pixel is 0x00BBGGRR (else 0x00RRGGBB) */
    u32 fb_in_ram;          /* framebuffer is ordinary write-back RAM */
    u32 dtb_from_file;      /* device tree came from the boot drive */
    u32 dtb_from_acpi;      /* device tree was built from ACPI tables */
    u64 acpi_rsdp;          /* ACPI 2.0 RSDP, if the firmware has one */
    u32 nfiles;
    struct efi_file_image files[EFI_MAX_FILES];
    char firmware[48];      /* vendor string, ASCII */
    u32 firmware_rev;
    u64 boot_epoch;         /* UEFI GetTime() as seconds since 1970 (UTC assumed), 0 if none */
    u64 boot_counter;       /* CNTVCT_EL0 when it was read */
    u32 uefi_rev;
    /* Updates (docs/updates.md, efi_update.c): set in a kernel the loader
     * started from /boot/kernel on the system disk. */
    u32 chained;            /* this kernel is an installed update */
    char chained_sha[65];   /* its SHA-256 (hex), for /boot/ok */
    char update_note[160];  /* what the loader decided about /boot/kernel, if anything */
};

/* Hidden, so code that runs in the UEFI stub before relocation
 * (efi_update.c) reaches it PC-relative instead of through the GOT. */
extern struct efi_handoff efi_handoff __attribute__((visibility("hidden")));

static inline bool efi_booted(void) { return efi_handoff.magic == EFI_HANDOFF_MAGIC; }
