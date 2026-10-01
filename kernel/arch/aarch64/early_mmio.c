#include "early_mmio.h"
#include <arch/paging.h>
#include <boot.h>
#include <debug/panic.h>
#include <mm/vmm.h>

/* Device mappings before the kernel has its own page tables (A4). Limine
 * maps the kernel and the direct map, but no device registers, so this code
 * adds 4 KiB entries to the tables Limine installed in TTBR1_EL1. The
 * tables it needs come from a static pool in .bss. vmm_init replaces
 * Limine's tables (A5); the mappings made here are for the early console. */

#define POOL_TABLES 4

static pte_t pool[POOL_TABLES][PT_ENTRIES] __aligned(PAGE_SIZE);
static unsigned pool_used;
static uintptr_t next_va = KMMIO_BASE + (63UL << 30);   /* the last GiB of KMMIO */

/* The mappings made, for early_mmio_install. Written before the scheduler
 * and the other CPUs run, read once by the boot CPU. */
#define MAX_EARLY 8
static struct {
    uintptr_t va, pa;
    size_t size;
} early[MAX_EARLY];
static unsigned nearly;

static uintptr_t image_phys(const void *p)
{
    return (uintptr_t)p - bootinfo.kernel_virt_base + bootinfo.kernel_phys_base;
}

/* The MAIR_EL1 index of a Device-nGnRnE or Device-nGnRE attribute. An index
 * that Limine left unused is 0x00, which is Device-nGnRnE. */
static unsigned device_attr_index(void)
{
    uint64_t mair;
    __asm__ volatile("mrs %0, mair_el1" : "=r"(mair));
    for (unsigned i = 0; i < 8; i++) {
        uint8_t a = (uint8_t)(mair >> (8 * i));
        if (a == 0x00 || a == 0x04)
            return i;
    }
    panic("early_mmio: MAIR_EL1 %lx has no device attribute", mair);
}

void *early_map_device(uintptr_t pa, size_t size)
{
    if (!hhdm_offset)
        return NULL;
    uintptr_t off = pa & (PAGE_SIZE - 1);
    size = ALIGN_UP(size + off, PAGE_SIZE);
    if (nearly == MAX_EARLY)
        panic("early_mmio: more than %d mappings", MAX_EARLY);
    uintptr_t base = next_va;
    next_va += size;
    early[nearly].va = base;
    early[nearly].pa = pa - off;
    early[nearly].size = size;
    nearly++;
    uint64_t ttbr1;
    __asm__ volatile("mrs %0, ttbr1_el1" : "=r"(ttbr1));
    unsigned attr = device_attr_index();
    for (size_t done = 0; done < size; done += PAGE_SIZE) {
        uintptr_t va = base + done;
        pte_t *table = P2V(ttbr1 & PTE_ADDR_MASK);
        for (int level = PT_LEVELS; level > 1; level--) {
            pte_t *e = &table[PT_INDEX(va, level)];
            if (!pte_present(*e)) {
                if (pool_used == POOL_TABLES)
                    panic("early_mmio: table pool exhausted");
                *e = pte_make_table(image_phys(pool[pool_used++]), false);
            } else if (!pte_is_table(*e)) {
                panic("early_mmio: %lx lies in a block mapping", va);
            }
            table = pte_table(*e);
        }
        table[PT_INDEX(va, 1)] = (pa - off + done) | PTE_VALID | PTE_TYPE_PAGE | PTE_ATTR(attr) |
                                 PTE_AF | PTE_PXN | PTE_UXN;
    }
    __asm__ volatile("dsb ishst; isb" : : : "memory");
    return (void *)(base + off);
}

void early_mmio_install(uintptr_t root)
{
    for (unsigned i = 0; i < nearly; i++) {
        for (size_t done = 0; done < early[i].size; done += PAGE_SIZE) {
            pte_t *e;
            if (paging_walk(root, early[i].va + done, true, &e) != 1)
                panic("early_mmio: no table for %lx in the kernel root", early[i].va + done);
            *e = pte_make(early[i].pa + done, VM_KERNEL_RW | VM_NOCACHE | VM_GLOBAL);
        }
    }
}
