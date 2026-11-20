/* Device discovery: the device tree describes the hardware (virtio-mmio
 * devices and PCI Express host bridges); a UEFI boot adds the firmware
 * framebuffer and files read from the boot drive. */
#include "block.h"
#include "efi.h"
#include "kernel.h"
#include "pci.h"
#include "virtio.h"

void simplefb_probe(void);
void nvme_scan(void);
void xhci_scan(void);

static void efi_files_attach(void)
{
    if (!efi_booted())
        return;
    for (u32 i = 0; i < efi_handoff.nfiles; i++) {
        const struct efi_file_image *f = &efi_handoff.files[i];
        struct blkdev *d = ramdisk_attach(f->name, (void *)(uintptr_t)f->base, f->size);
        if (!d)
            continue;
        char sz[24];
        blk_format_size(f->size, sz, sizeof(sz));
        printk("  %s: %s from the boot drive, %s (in memory; changes are not saved)\n", f->name,
               f->file, sz);
    }
}

void devices_init(void)
{
    printk("Devices:\n");
    simplefb_probe(); /* firmware framebuffer: the console appears early */
    virtio_scan();
    pci_init();
    virtio_pci_scan();
    nvme_scan();
    xhci_scan();
    efi_files_attach();
}
