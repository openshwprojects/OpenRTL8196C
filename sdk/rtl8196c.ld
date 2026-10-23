/*
 * rtl8196c.ld - link a raw RAM image for the Realtek boot loader
 *
 * The image is written with EW to LOAD_ADDR and started with "J LOAD_ADDR",
 * so byte 0 must be the first instruction of _start (section .entry).
 * The default 0xA0500000 is the uncached (KSEG1) alias of physical
 * 0x00500000, which the loader itself uses for the Linux image, so it is
 * guaranteed free RAM.  Uncached execution is slow but immune to I-cache
 * staleness; switch to 0x80500000 once cache handling is proven.
 */
OUTPUT_ARCH(mips)
ENTRY(_start)

LOAD_ADDR  = DEFINED(LOAD_ADDR)  ? LOAD_ADDR  : 0xA0500000;

SECTIONS
{
    . = LOAD_ADDR;

    .text : {
        KEEP(*(.entry))
        *(.text .text.*)
    }

    .rodata ALIGN(16) : {
        *(.rodata .rodata.*)
    }

    .data ALIGN(16) : {
        *(.data .data.*)
        *(.sdata .sdata.*)
    }

    . = ALIGN(16);
    _bss_start = .;
    .bss (NOLOAD) : {
        *(.bss .bss.*)
        *(.sbss .sbss.*)
        *(COMMON)
    }
    . = ALIGN(16);
    _bss_end = .;
    _end = .;

    _stack_top = DEFINED(STACK_TOP) ? STACK_TOP : 0xA06F0000;

    /DISCARD/ : {
        *(.comment) *(.note*) *(.MIPS.abiflags) *(.reginfo) *(.pdr)
        *(.mdebug*) *(.gnu.attributes) *(.eh_frame*) *(.MIPS.options)
    }
}
