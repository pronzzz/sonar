/*
 * Sonar kernel linker script. The kernel is a static PIE linked at 0 and
 * relocated at boot (boot.S), so the load address is up to the bootloader.
 */
OUTPUT_ARCH(aarch64)
ENTRY(_start)

SECTIONS
{
    . = 0;
    __image_start = .;

    .text : {
        __text_start = .;
        KEEP(*(.text.boot))
        . = ALIGN(2048);
        KEEP(*(.text.vectors))
        . = ALIGN(4096);
        __efi_text_start = .;   /* PE/COFF code section (headers before it) */
        *(.text .text.*)
        __text_end = .;
    }

    . = ALIGN(4096);
    __efi_data_start = .;       /* PE/COFF data section: everything after code */
    .shellcmds : ALIGN(8) {
        __shellcmd_start = .;
        KEEP(*(SORT(.rodata.shellcmd.*)))
        __shellcmd_end = .;
    }
    .harness : ALIGN(8) {
        __harnesstool_start = .;
        KEEP(*(SORT(.rodata.harnesstool.*)))
        __harnesstool_end = .;
        . = ALIGN(8);
        __harnesslistener_start = .;
        KEEP(*(SORT(.rodata.harnesslistener.*)))
        __harnesslistener_end = .;
    }
    .rodata : { *(.rodata .rodata.*) }
    .data.rel.ro : { *(.data.rel.ro .data.rel.ro.*) }
    .dynsym : { *(.dynsym) }
    .dynstr : { *(.dynstr) }
    .hash : { *(.hash) }
    .gnu.hash : { *(.gnu.hash) }

    . = ALIGN(8);
    .rela.dyn : {
        __rela_start = .;
        *(.rela .rela.*)
        __rela_end = .;
    }

    . = ALIGN(8);
    .dynamic : { *(.dynamic) }
    .got : { *(.got .got.*) }

    . = ALIGN(4096);
    .data : {
        *(.data .data.*)
        . = ALIGN(4096);        /* the file ends page-aligned (PE FileAlignment) */
        __data_end = .;
    }

    . = ALIGN(4096);
    .bss (NOLOAD) : {
        __bss_start = .;
        *(.bss.stack)
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(16);
        __bss_end = .;
    }

    . = ALIGN(4096);
    __image_end = .;
    __image_size = __image_end - __image_start;

    /* PE/COFF header fields (boot.S): offsets from the image start. */
    __efi_text_rva = __efi_text_start - __image_start;
    __efi_text_size = __efi_data_start - __efi_text_start;
    __efi_data_rva = __efi_data_start - __image_start;
    __efi_data_file_size = __data_end - __efi_data_start;
    __efi_data_mem_size = __image_end - __efi_data_start;
    __efi_bss_size = __image_end - __data_end;
    __efi_entry_rva = efi_entry - __image_start;

    /DISCARD/ : {
        *(.comment)
        *(.note .note.*)
        *(.eh_frame .eh_frame_hdr)
        *(.interp)
    }
}
