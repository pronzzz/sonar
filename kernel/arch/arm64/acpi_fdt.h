/* Device tree from ACPI tables (acpi_fdt.c), used by the UEFI loader. */
#pragma once
#include "types.h"

/* Returns the address of a new flattened device tree, or 0. alloc returns
 * memory that stays reserved for the kernel; log prints progress. */
u64 acpi_build_fdt(u64 rsdp, void *(*alloc)(u64 size), void (*log)(const char *));
