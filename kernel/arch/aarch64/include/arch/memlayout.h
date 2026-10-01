#pragma once

/* Virtual address layout of aarch64 with a 48 bit address space and 4 KiB
 * granules (docs/design/arch.md). TTBR0 maps user space, TTBR1 the kernel
 * half from 0xffff000000000000. The user regions have the addresses of
 * x86_64. */

#define HIGHER_HALF_BASE 0xffff000000000000UL   /* start of the TTBR1 range */
#define KERNEL_VBASE     0xffffffff80000000UL   /* kernel image, set by the linker script */

#define USER_BASE        0x0000000000001000UL
#define USER_TOP         0x00007fffffffffffUL
#define USER_STACK_TOP   0x00007ffffffff000UL
#define USER_MMAP_TOP    0x00007f0000000000UL   /* mmap regions grow down from here */
#define USER_INTERP_BASE 0x00007e0000000000UL   /* the dynamic loader of a program (dynlink.md) */

#define KMMIO_BASE       0xffffffa000000000UL   /* device mappings, 64 GiB */
#define KMMIO_SIZE       (64UL << 30)
#define KSTACK_BASE      0xffffffc000000000UL   /* kernel stacks with guards */
#define KHEAP_BASE       0xffffffd000000000UL   /* reserved for vmalloc style use */
