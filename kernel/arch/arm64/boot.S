/*
 * Sonar kernel entry point (AArch64).
 *
 * The image starts with a standard arm64 "Image" header so that any
 * Linux-compatible loader can boot it: QEMU -kernel, U-Boot booti,
 * m1n1 (Apple Silicon) and UEFI-stub style loaders. Boot protocol
 * (Documentation/arch/arm64/booting.rst):
 *   x0 = physical address of the device tree blob
 *   MMU off, D-cache off, image cleaned to the point of coherency,
 *   entered at EL2 or EL1, all interrupts masked.
 *
 * The kernel is linked position-independent at address 0 and applies its
 * own R_AARCH64_RELATIVE relocations, so it runs wherever it is loaded.
 */

.macro adr_l reg, sym
    adrp    \reg, \sym
    add     \reg, \reg, :lo12:\sym
.endm

/* x0 = start, x1 = end; invalidates D-cache lines covering [start, end). */
.macro dcache_inval_poc_range
    mrs     x3, ctr_el0
    ubfx    x3, x3, #16, #4         // DminLine: log2(words per line)
    mov     x2, #4
    lsl     x2, x2, x3              // line size in bytes
    sub     x3, x2, #1
    bic     x0, x0, x3
1:  dc      ivac, x0
    add     x0, x0, x2
    cmp     x0, x1
    b.lo    1b
    dsb     sy
.endm

/* x0 = start, x1 = end; cleans and invalidates D-cache lines to the PoC. */
.macro dcache_clean_inval_poc_range
    mrs     x3, ctr_el0
    ubfx    x3, x3, #16, #4
    mov     x2, #4
    lsl     x2, x2, x3
    sub     x3, x2, #1
    bic     x0, x0, x3
1:  dc      civac, x0
    add     x0, x0, x2
    cmp     x0, x1
    b.lo    1b
    dsb     sy
.endm

    .section .text.boot, "ax"
    .global _start
_start:
    /*
     * code0 is "MZ" for UEFI (the DOS header magic of a PE/COFF image);
     * as an instruction it is "ccmp x18, #0, #0xd, pl", which only changes
     * flags. Linux uses the same trick.
     */
    .inst   0xfa405a4d              // code0: "MZ"
    b       primary_entry           // code1
    .quad   0                       // text_offset
    .quad   __image_size            // image_size (includes .bss)
    .quad   0xa                     // flags: LE | 4K pages | anywhere in RAM
    .quad   0                       // res2
    .quad   0                       // res3
    .quad   0                       // res4
    .ascii  "ARM\x64"               // magic
    .long   pe_header - _start      // res5: PE header offset (DOS e_lfanew)

/*
 * PE/COFF header (Microsoft PE format specification), so UEFI firmware can
 * start the same file as an EFI application (efi_entry below). Two
 * sections: code (read/execute) and everything after it (read/write), so
 * firmware that enforces W^X on loaded images is satisfied. No base
 * relocations are listed: the kernel relocates itself.
 */
    .balign 8
pe_header:
    .ascii  "PE\0\0"
    .short  0xaa64                  // Machine: ARM64
    .short  2                       // NumberOfSections
    .long   0                       // TimeDateStamp
    .long   0                       // PointerToSymbolTable
    .long   0                       // NumberOfSymbols
    .short  section_table - optional_header // SizeOfOptionalHeader
    .short  0x0206                  // Characteristics: executable, no line numbers/debug info
optional_header:
    .short  0x020b                  // Magic: PE32+
    .byte   0x02, 0x14              // linker version (informational)
    .long   __efi_text_size         // SizeOfCode
    .long   __efi_data_file_size    // SizeOfInitializedData
    .long   __efi_bss_size          // SizeOfUninitializedData
    .long   __efi_entry_rva         // AddressOfEntryPoint
    .long   __efi_text_rva          // BaseOfCode
    .quad   0                       // ImageBase
    .long   0x1000                  // SectionAlignment
    .long   0x1000                  // FileAlignment
    .short  0, 0                    // OS version
    .short  0, 1                    // image version
    .short  0, 0                    // subsystem version
    .long   0                       // Win32VersionValue
    .long   __image_size            // SizeOfImage
    .long   __efi_text_rva          // SizeOfHeaders
    .long   0                       // CheckSum
    .short  10                      // Subsystem: EFI application
    .short  0x0100                  // DllCharacteristics: NX compatible
    .quad   0, 0, 0, 0              // stack/heap reserve and commit
    .long   0                       // LoaderFlags
    .long   6                       // NumberOfRvaAndSizes
    .quad   0                       // export table
    .quad   0                       // import table
    .quad   0                       // resource table
    .quad   0                       // exception table
    .quad   0                       // certificate table
    .quad   0                       // base relocation table
section_table:
    .ascii  ".text\0\0\0"
    .long   __efi_text_size         // VirtualSize
    .long   __efi_text_rva          // VirtualAddress
    .long   __efi_text_size         // SizeOfRawData
    .long   __efi_text_rva          // PointerToRawData
    .long   0, 0                    // relocations, line numbers
    .short  0, 0
    .long   0x60000020              // code | execute | read
    .ascii  ".data\0\0\0"
    .long   __efi_data_mem_size     // VirtualSize (includes .bss)
    .long   __efi_data_rva          // VirtualAddress
    .long   __efi_data_file_size    // SizeOfRawData
    .long   __efi_data_rva          // PointerToRawData
    .long   0, 0
    .short  0, 0
    .long   0xc0000040              // initialized data | read | write

primary_entry:
    mov     x19, x0                 // x19 = DTB
    adr     x20, _start             // x20 = load address (link base is 0)

    mrs     x21, CurrentEL
    lsr     x21, x21, #2
    and     x21, x21, #3            // x21 = entry exception level
    cmp     x21, #2
    b.ne    1f
    bl      el2_to_el1
1:  bl      cpu_enable_fp

    /*
     * Drop any stale cache lines over the whole image (code, data, bss).
     * We are about to write .data (relocations) and .bss with the MMU off,
     * i.e. with non-cacheable accesses; stale lines would otherwise shadow
     * those writes once caches come on.
     */
    adr_l   x0, __image_start
    adr_l   x1, __image_end
    dcache_inval_poc_range

    /* Apply relocations: *(base + r_offset) = base + r_addend */
    adr_l   x1, __rela_start
    adr_l   x2, __rela_end
2:  cmp     x1, x2
    b.hs    3f
    ldp     x3, x4, [x1], #16       // r_offset, r_info
    ldr     x5, [x1], #8            // r_addend
    add     x5, x5, x20
    str     x5, [x20, x3]           // build checks that all relocs are RELATIVE
    b       2b
3:
    /* Zero .bss (8-byte aligned by the linker script). */
    adr_l   x1, __bss_start
    adr_l   x2, __bss_end
4:  cmp     x1, x2
    b.hs    5f
    str     xzr, [x1], #8
    b       4b
5:
    adr_l   x1, boot_stack_top
    mov     sp, x1
    mov     x0, x19                 // dtb
    mov     x1, x20                 // load base
    mov     x2, x21                 // entry EL
    bl      early_main              // never returns
6:  wfe
    b       6b

/*
 * Leave EL2 for EL1. Called with bl; returns to the caller at EL1 using
 * x30 as the exception return address. Clobbers x0.
 */
el2_to_el1:
    mov     x0, #(1 << 31)          // HCR_EL2.RW: EL1 is AArch64
    msr     hcr_el2, x0
    mrs     x0, cnthctl_el2
    orr     x0, x0, #3              // EL1PCTEN | EL1PCEN: counters/timers to EL1
    msr     cnthctl_el2, x0
    msr     cntvoff_el2, xzr
    mov     x0, #0x33ff             // CPTR_EL2: RES1 bits, no FP/SIMD trap
    msr     cptr_el2, x0
    msr     hstr_el2, xzr
    mrs     x0, id_aa64pfr0_el1
    ubfx    x0, x0, #24, #4         // GIC system register interface present?
    cbz     x0, 1f
    mrs     x0, S3_4_C12_C9_5       // ICC_SRE_EL2
    orr     x0, x0, #0xf            // SRE | DFB | DIB | Enable (EL1 may use SRE)
    msr     S3_4_C12_C9_5, x0
    isb
    msr     S3_4_C12_C11_0, xzr     // ICH_HCR_EL2 = 0
1:  mrs     x0, midr_el1
    msr     vpidr_el2, x0
    mrs     x0, mpidr_el1
    msr     vmpidr_el2, x0
    movz    x0, #0x0800             // SCTLR_EL1 RES1 bits, MMU/caches off
    movk    x0, #0x30d0, lsl #16
    msr     sctlr_el1, x0
    mov     x0, #0x3c5              // SPSR: EL1h, DAIF masked
    msr     spsr_el2, x0
    msr     elr_el2, x30
    isb
    eret

cpu_enable_fp:
    mov     x0, #(3 << 20)          // CPACR_EL1.FPEN: no FP/SIMD traps
    msr     cpacr_el1, x0
    isb
    ret

/*
 * Secondary CPU entry, started through PSCI CPU_ON with x0 = pointer to
 * its struct cpu_boot (physical == virtual, cleaned to PoC by the boot CPU):
 *   [0]  stack top      [8]  MAIR_EL1     [16] TCR_EL1
 *   [24] TTBR0_EL1      [32] SCTLR_EL1    [40] C entry point
 */
    .global secondary_entry
secondary_entry:
    mov     x19, x0
    mrs     x0, CurrentEL
    lsr     x0, x0, #2
    and     x0, x0, #3
    cmp     x0, #2
    b.ne    1f
    bl      el2_to_el1
1:  bl      cpu_enable_fp
    ldr     x0, [x19, #8]
    msr     mair_el1, x0
    ldr     x0, [x19, #16]
    msr     tcr_el1, x0
    ldr     x0, [x19, #24]
    msr     ttbr0_el1, x0
    isb
    tlbi    vmalle1
    dsb     nsh
    isb
    ldr     x0, [x19, #32]
    msr     sctlr_el1, x0
    isb
    ldr     x0, [x19, #0]
    mov     sp, x0
    ldr     x1, [x19, #40]
    mov     x0, x19
    blr     x1                      // never returns
2:  wfe
    b       2b

    .section .bss.stack, "aw", %nobits
    .balign 16
boot_stack:
    .space  64 * 1024
    .global boot_stack_top
boot_stack_top:

/*
 * UEFI entry point (the PE AddressOfEntryPoint): x0 = image handle,
 * x1 = system table, MMU on with the firmware's identity map. efi_main()
 * gathers what the kernel needs and exits boot services; then the MMU and
 * D-cache are turned off and the kernel is entered like any arm64 Image
 * (booting.rst): image and device tree cleaned to the PoC, x0 = DTB.
 * This lives in .text (not .text.boot) so it is inside the PE code section.
 */
    .section .text.efi, "ax"
    .global efi_entry
efi_entry:
    stp     x29, x30, [sp, #-16]!
    mov     x29, sp
    bl      efi_main
    cbnz    x0, 1f
    ldp     x29, x30, [sp], #16
    movz    x0, #1                  // EFI_LOAD_ERROR: back to the firmware
    movk    x0, #0x8000, lsl #48
    ret

1:  mov     x19, x0                 // device tree
    msr     daifset, #0xf
    adr_l   x0, __image_start
    adr_l   x1, __image_end
    dcache_clean_inval_poc_range
    ldr     w1, [x19, #4]           // fdt totalsize (big-endian)
    rev     w1, w1
    mov     x0, x19
    add     x1, x0, x1
    dcache_clean_inval_poc_range

    mrs     x0, CurrentEL
    cmp     x0, #(2 << 2)
    b.ne    2f
    mrs     x0, sctlr_el2
    bic     x0, x0, #(1 << 0)       // M: MMU off
    bic     x0, x0, #(1 << 2)       // C: D-cache off
    msr     sctlr_el2, x0
    b       3f
2:  mrs     x0, sctlr_el1
    bic     x0, x0, #(1 << 0)
    bic     x0, x0, #(1 << 2)
    msr     sctlr_el1, x0
3:  isb
    ic      iallu
    dsb     nsh
    isb
    mov     x0, x19
    b       primary_entry
