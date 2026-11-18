/* probe.ld - the probe PS-EXE loads at 0x80010000; probe_out is the free
 * RAM after the image. */
ENTRY(_start)
SECTIONS
{
    . = 0x80010000;
    .text : { *(.text.start) *(.text*) }
    .rodata : { *(.rodata*) }
    .data : { *(.data*) *(.sdata*) }
    .bss : { *(.bss*) *(.sbss*) *(COMMON) }
    . = ALIGN(16);
    probe_out = .;
    /DISCARD/ : { *(.MIPS.abiflags) *(.reginfo) *(.comment) *(.pdr) *(.gnu.attributes) }
}
