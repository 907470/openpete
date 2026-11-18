/* probe.ld - the SPU envelope probe loads at 0x80010000 as one image:
 * code, read-only data and initialised data; it has no .bss. */
OUTPUT_ARCH(mips)
ENTRY(_start)
SECTIONS
{
	. = 0x80010000;
	.text : { *(.text.start) *(.text*) }
	.rodata : { *(.rodata*) }
	.data : { *(.data*) *(.sdata*) *(.sbss*) *(.bss*) *(COMMON) }
	/DISCARD/ : { *(.MIPS.abiflags) *(.reginfo) *(.pdr) *(.comment) *(.gnu.attributes) }
}
