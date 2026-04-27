/*
 * Bootloader entry. QEMU "virt" with no -kernel starts the CPU at 0x0,
 * which is pflash0, in SVC mode with MMU and caches off.
 *
 * The vector table stays at 0x0 (VBAR reset value) after the app is launched,
 * so if the app takes an undefined-instruction or abort exception, it lands
 * in the bootloader's fault handler, which reports it and resets the board.
 */
    .syntax unified
    .arm

    .section .vectors, "ax"
    .global _start
_start:
    b   reset_handler
    b   undef_entry
    b   svc_entry
    b   pabt_entry
    b   dabt_entry
    b   .
    b   irq_entry
    b   fiq_entry

    .text
reset_handler:
    cpsid   aif

    /* per-mode stacks */
    cps     #0x1b               /* UND */
    ldr     sp, =_und_stack_top
    cps     #0x17               /* ABT */
    ldr     sp, =_abt_stack_top
    cps     #0x12               /* IRQ */
    ldr     sp, =_irq_stack_top
    cps     #0x11               /* FIQ */
    ldr     sp, =_irq_stack_top
    cps     #0x13               /* SVC */
    ldr     sp, =_svc_stack_top

    /* copy .data from flash to RAM */
    ldr     r0, =_data_load
    ldr     r1, =_data_start
    ldr     r2, =_data_end
1:  cmp     r1, r2
    ldrlo   r3, [r0], #4
    strlo   r3, [r1], #4
    blo     1b

    /* zero .bss */
    ldr     r1, =_bss_start
    ldr     r2, =_bss_end
    mov     r3, #0
2:  cmp     r1, r2
    strlo   r3, [r1], #4
    blo     2b

    bl      bl_main
    b       .

/* r0 = exception kind, r1 = faulting PC */
undef_entry:
    sub     r1, lr, #4
    mov     r0, #1
    b       fault_handler
svc_entry:
    sub     r1, lr, #4
    mov     r0, #2
    b       fault_handler
pabt_entry:
    sub     r1, lr, #4
    mov     r0, #3
    b       fault_handler
dabt_entry:
    sub     r1, lr, #8
    mov     r0, #4
    b       fault_handler
irq_entry:
    sub     r1, lr, #4
    mov     r0, #5
    b       fault_handler
fiq_entry:
    sub     r1, lr, #4
    mov     r0, #6
    b       fault_handler

/* void jump_to_app(uint32_t entry, void *boot_info) */
    .global jump_to_app
jump_to_app:
    mov     r4, r0
    mov     r0, r1
    mov     r1, #0
    mcr     p15, 0, r1, c7, c5, 0   /* ICIALLU: new code was just copied to RAM */
    dsb
    isb
    bx      r4
