/* Kernel entry from Limine at EL1 with the MMU on and DAIF masked. Switch
 * to SP_EL1 and the kernel's own boot stack, clear the frame pointer so backtraces
 * terminate, and enter kmain. */
    .section .text
    .global _start
    .type _start, %function
_start:
    msr daifset, #0xf
    /* Limine may enter with SPSel = 0 (EL1t). The kernel runs on SP_EL1,
     * which the CPU also selects on every exception. */
    msr spsel, #1
    adrp x0, boot_stack_top
    add x0, x0, :lo12:boot_stack_top
    mov sp, x0
    mov x29, #0
    mov x30, #0
    bl kmain
1:  wfi
    b 1b
    .size _start, . - _start

/* First page is a guard that vmm_init unmaps, followed by 16 KiB of stack. */
    .section .bss
    .balign 4096
    .global boot_stack
    .global boot_stack_top
boot_stack:
    .skip 4096
    .skip 16384
boot_stack_top:
