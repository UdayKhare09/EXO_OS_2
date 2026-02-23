OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)

ENTRY(kmain)

PHDRS {
    null    PT_NULL    FLAGS(0);
    text    PT_LOAD    FLAGS((1 << 0) | (1 << 2));   /* R-X */
    rodata  PT_LOAD    FLAGS((1 << 2));               /* R-- */
    data    PT_LOAD    FLAGS((1 << 1) | (1 << 2));   /* RW- */
}

KERNEL_VMA = 0xffffffff80000000;

SECTIONS {
    . = KERNEL_VMA + 0x200000;

    kernel_start = .;

    .text : {
        *(.text .text.*)
    } :text

    . = ALIGN(CONSTANT(MAXPAGESIZE));

    .rodata : {
        *(.rodata .rodata.*)
    } :rodata

    . = ALIGN(CONSTANT(MAXPAGESIZE));

    .data : {
        *(.data .data.*)
        /* Limine protocol request blocks — must be in a writable loadable
         * segment so that Limine can zero limine_base_revision[2] to
         * signal that the requested revision is supported.               */
        KEEP(*(.limine_requests_start_marker))
        KEEP(*(.limine_requests))
        KEEP(*(.limine_requests_end_marker))
    } :data

    .bss : {
        . = ALIGN(16);
        *(.bss .bss.*)
        *(COMMON)
    } :data

    kernel_end = .;

    /DISCARD/ : {
        *(.note .note.*)
        *(.eh_frame)
        *(.eh_frame_hdr)
    }
}
