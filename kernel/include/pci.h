/* PCI Express: configuration space through ECAM, BARs, capabilities, MSI-X. */
#pragma once
#include "irq.h"
#include "types.h"

struct pci_dev {
    u8 bus, dev, fn;
    u16 vendor, device;
    u8 class_code, subclass, prog_if, revision;
    uintptr_t cfg;   /* this function's 4 KiB of configuration space */
    u64 bar[6];      /* physical addresses (64-bit BARs use two slots) */
    u64 bar_size[6];
    char name[16];   /* "00:05.0" */
};

void pci_init(void);                /* find host bridges, list functions */
int pci_count(void);
struct pci_dev *pci_get(int i);

u8 pci_read8(struct pci_dev *d, u32 off);
u16 pci_read16(struct pci_dev *d, u32 off);
u32 pci_read32(struct pci_dev *d, u32 off);
void pci_write16(struct pci_dev *d, u32 off, u16 v);
void pci_write32(struct pci_dev *d, u32 off, u32 v);

/* Turn on memory decoding and bus mastering (DMA). */
void pci_enable(struct pci_dev *d);
/* Map BAR i; returns its virtual address (identity) or 0. */
uintptr_t pci_map_bar(struct pci_dev *d, int i);
/* Offset of the first (or next after `after`, 0 to start) capability with
 * this ID, or 0. */
u8 pci_find_cap(struct pci_dev *d, u8 id, u8 after);

/* MSI-X: route vectors [0, n) to new interrupts with this handler (ctx is
 * passed through). Returns the number of vectors set up (0 = none: the
 * device has no MSI-X or the platform has no MSI controller). */
int pci_msix_setup(struct pci_dev *d, int n, irq_handler_t fn, void **ctxs);
bool pci_have_msi(void);

#define PCI_CAP_MSIX 0x11
#define PCI_CAP_VENDOR 0x09
