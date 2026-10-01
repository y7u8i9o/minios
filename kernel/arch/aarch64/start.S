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

/* Entry of an application processor from Limine (A8), with x0 pointing to
 * its struct limine_mp_info, on Limine's stack and translation tables.
 * Install the translation state that smp_park_aps prepared in ap_boot,
 * select SP_EL1 on the kernel stack of the processor and enter ap_main
 * with its struct cpu. The barrier orders the reads of ap_boot after the
 * read of goto_address with which Limine released the processor. */
    .section .text
    .global ap_entry
    .type ap_entry, %function
ap_entry:
    msr daifset, #0xf
    dmb ish
    msr spsel, #1
    adrp x1, ap_boot
    add x1, x1, :lo12:ap_boot
    ldp x2, x3, [x1]                    /* MAIR_EL1, TCR_EL1 */
    msr mair_el1, x2
    msr tcr_el1, x3
    isb
    ldp x2, x3, [x1, #16]               /* TTBR1_EL1, TTBR0_EL1 */
    msr ttbr1_el1, x2
    msr ttbr0_el1, x3
    isb
    tlbi vmalle1
    dsb nsh
    isb
    ldp x2, x0, [x1, #32]               /* stack top, struct cpu */
    mov sp, x2
    mov x29, #0
    mov x30, #0
    bl ap_main
1:  wfi
    b 1b
    .size ap_entry, . - ap_entry
