# crt0.S - entry of the probe PS-EXE: stack, then probe_main; the case
# list follows the code.
    .set noreorder
    .section .text.start
    .globl _start
_start:
    la    $sp, 0x801FFF00
    la    $gp, 0
    jal   probe_main
    nop
1:  j     1b
    nop

    .section .rodata.cases
    .balign 4
    .globl probe_cases
probe_cases:
    .incbin "cases.bin"
