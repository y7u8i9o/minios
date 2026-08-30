/* Kernel entry from Limine. Switch to the kernel's own boot stack, clear the
 * frame pointer so backtraces terminate, and enter kmain. */
    .section .text
    .global _start
    .type _start, @function
_start:
    cli
    leaq boot_stack_top(%rip), %rsp
    xorl %ebp, %ebp
    call kmain
1:  cli
    hlt
    jmp 1b
    .size _start, . - _start

/* First page is a guard that vmm_init unmaps, followed by 16 KiB of stack. */
    .section .bss
    .align 4096
    .global boot_stack
    .global boot_stack_top
boot_stack:
    .skip 4096
    .skip 16384
boot_stack_top:
