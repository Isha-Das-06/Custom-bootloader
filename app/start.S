/*
 * Application entry. The bootloader has already copied us to our load
 * address in RAM (so .data is in place) and passes &boot_info in r0.
 */
    .syntax unified
    .arm
    .section .text.start, "ax"
    .global _start
_start:
    ldr     sp, =_stack_top
    mov     r4, r0

    ldr     r1, =_bss_start
    ldr     r2, =_bss_end
    mov     r3, #0
1:  cmp     r1, r2
    strlo   r3, [r1], #4
    blo     1b

    mov     r0, r4
    bl      app_main
    b       .
