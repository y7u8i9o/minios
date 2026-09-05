#include <arch/gdt.h>
#include <arch/cpu.h>
#include <lib/string.h>

struct tss {
    uint32_t reserved0;
    uint64_t rsp0;
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist[7];
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
} __packed;

struct gdt_ptr {
    uint16_t limit;
    uint64_t base;
} __packed;

/* Selector layout is fixed by the syscall/sysret conventions in M8:
 * STAR sysret base 0x13 requires user data at 0x18 and user code at 0x20.
 * Every CPU has its own table and TSS because rsp0 and the double fault
 * stack are per CPU. Written by the owning CPU only. */
struct cpu_tables {
    uint64_t gdt[7] __aligned(16);
    struct tss tss __aligned(16);
    uint8_t df_stack[8192] __aligned(16);   /* double fault stack, IST1 */
};
static struct cpu_tables tables[MAX_CPUS];
extern char boot_stack_top[];

static uint64_t gdt_entry(uint8_t access, uint8_t flags)
{
    /* Base and limit are ignored in long mode for code and data segments,
     * but a full limit keeps 32 bit compatibility checks happy. */
    return 0xffffULL | ((uint64_t)0x0f << 48) | ((uint64_t)access << 40) |
           ((uint64_t)flags << 52);
}

void tss_set_rsp0(uintptr_t rsp0)
{
    tables[cpu_current()->id].tss.rsp0 = rsp0;
}

uintptr_t tss_get_rsp0(void)
{
    return tables[cpu_current()->id].tss.rsp0;
}

void gdt_init(void)
{
    gdt_init_cpu(0, (uintptr_t)boot_stack_top);
}

void gdt_init_cpu(unsigned id, uintptr_t rsp0)
{
    uint64_t *gdt = tables[id].gdt;
    struct tss *tss = &tables[id].tss;
    gdt[0] = 0;
    gdt[1] = gdt_entry(0x9a, 0xa);   /* kernel code: present, ring 0, exec/read, long */
    gdt[2] = gdt_entry(0x92, 0xc);   /* kernel data: present, ring 0, read/write */
    gdt[3] = gdt_entry(0xf2, 0xc);   /* user data:   present, ring 3, read/write */
    gdt[4] = gdt_entry(0xfa, 0xa);   /* user code:   present, ring 3, exec/read, long */

    memset(tss, 0, sizeof *tss);
    tss->rsp0 = rsp0;
    tss->ist[0] = (uint64_t)tables[id].df_stack + sizeof tables[id].df_stack;
    tss->iomap_base = sizeof *tss;

    uint64_t base = (uint64_t)tss;
    uint64_t limit = sizeof *tss - 1;
    gdt[5] = (limit & 0xffff) | ((base & 0xffffff) << 16) | (0x89ULL << 40) |
             (((limit >> 16) & 0xf) << 48) | (((base >> 24) & 0xff) << 56);
    gdt[6] = base >> 32;

    struct gdt_ptr ptr = { sizeof tables[id].gdt - 1, (uint64_t)gdt };
    __asm__ volatile(
        "lgdt %0\n"
        "pushq %1\n"
        "leaq 1f(%%rip), %%rax\n"
        "pushq %%rax\n"
        "lretq\n"
        "1:\n"
        "movl %2, %%eax\n"
        "movl %%eax, %%ds\n"
        "movl %%eax, %%es\n"
        "movl %%eax, %%ss\n"
        "xorl %%eax, %%eax\n"
        "movl %%eax, %%fs\n"
        "movl %%eax, %%gs\n"
        "movl %3, %%eax\n"
        "ltr %%ax\n"
        : : "m"(ptr), "i"(GDT_KERNEL_CODE), "i"(GDT_KERNEL_DATA), "i"(GDT_TSS)
        : "rax", "memory");
}
