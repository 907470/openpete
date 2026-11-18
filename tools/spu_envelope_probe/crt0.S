# crt0.S - entry of the SPU envelope probe PS-EXE: interrupts off, a stack
# at the top of main RAM, then main(), which never returns.
	.set noreorder
	.section .text.start, "ax"
	.globl _start
_start:
	lui   $t0, 0x1F80
	sw    $zero, 0x1074($t0)      # I_MASK: no interrupt source
	mfc0  $t1, $12
	li    $t2, -2
	and   $t1, $t1, $t2           # SR.IEc = 0
	lui   $t2, 0x4000
	or    $t1, $t1, $t2           # SR.CU2: the GTE is usable
	mtc0  $t1, $12
	nop
	li    $sp, 0x801FFF00
	move  $gp, $zero
	jal   main
	nop
1:	b     1b
	nop
