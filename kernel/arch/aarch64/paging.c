#define KLOG_SUBSYS "paging"
#include <arch/paging.h>
#include <arch/cpu.h>
#include <mm/vmm.h>
#include <boot.h>
#include <sync/spinlock.h>
#include <lib/string.h>
#include <klog.h>
#include <debug/panic.h>
#include "early_mmio.h"

/* The aarch64 part of the page tables (A5): the entry format, the two
 * roots and the TLB. The kernel root is loaded in TTBR1_EL1 once, by the
 * first activation of the kernel space. A user root is loaded in
 * TTBR0_EL1 with the ASID of its space. While the kernel space is active,
 * TTBR0_EL1 points to an empty table, so user addresses fault. The table
 * walks are generic (mm/pgtable.c). */

pte_t pte_make(uintptr_t pa, unsigned flags)
{
    pte_t e = pa | PTE_VALID | PTE_TYPE_PAGE | PTE_AF;
    if (flags & VM_NOCACHE)
        e |= PTE_ATTR(MAIR_IDX_DEVICE);
    else if (flags & VM_WC)
        e |= PTE_ATTR(MAIR_IDX_NC) | PTE_SH_INNER;
    else
        e |= PTE_ATTR(MAIR_IDX_NORMAL) | PTE_SH_INNER;
    if (flags & VM_WRITE)
        e |= PTE_WRITE;             /* clean: PTE_RDONLY below until written */
    e |= PTE_RDONLY;
    if (flags & VM_USER) {
        e |= PTE_USER | PTE_NG | PTE_PXN;
        if (!(flags & VM_EXEC))
            e |= PTE_UXN;
    } else {
        e |= PTE_UXN;
        if (!(flags & VM_EXEC))
            e |= PTE_PXN;
    }
    if (!(flags & VM_GLOBAL) && !(flags & VM_USER))
        e |= PTE_NG;
    /* Kernel mappings are dirty from the start: nothing tracks their
     * writes. */
    if ((flags & VM_WRITE) && !(flags & VM_USER))
        e &= ~PTE_RDONLY;
    return e;
}

unsigned pte_vm_flags(pte_t e)
{
    unsigned flags = VM_READ;
    if (e & PTE_WRITE)
        flags |= VM_WRITE;
    if (e & PTE_USER) {
        flags |= VM_USER;
        if (!(e & PTE_UXN))
            flags |= VM_EXEC;
    } else if (!(e & PTE_PXN)) {
        flags |= VM_EXEC;
    }
    unsigned attr = (e & PTE_ATTR_MASK) >> 2;
    if (attr == MAIR_IDX_DEVICE)
        flags |= VM_NOCACHE;
    else if (attr == MAIR_IDX_NC)
        flags |= VM_WC;
    if (!(e & PTE_NG))
        flags |= VM_GLOBAL;
    return flags;
}

/* MAIR_EL1 attributes of the indices in <arch/paging.h>: write-back
 * normal memory, Device-nGnRE, normal non-cacheable. Index 0 equals the
 * attribute Limine maps memory with, so the change is compatible with the
 * tables in use until the kernel root is loaded. */
#define MAIR_VALUE ((0xffUL << (8 * MAIR_IDX_NORMAL)) | (0x04UL << (8 * MAIR_IDX_DEVICE)) | \
                    (0x44UL << (8 * MAIR_IDX_NC)))

#define TCR_AS  (1UL << 36)     /* 16 bit ASIDs */
#define TCR_HA  (1UL << 39)     /* hardware access flag */
#define TCR_HD  (1UL << 40)     /* hardware dirty state */

/* The table TTBR0_EL1 points to while no user space is active. */
static pte_t empty_root[PT_ENTRIES] __aligned(PAGE_SIZE);
static uintptr_t kernel_root;
static bool hw_access_flag, hw_dirty;

/* ASIDs of the user roots. ASID 0 belongs to the empty root. A root
 * without an ASID (all in use) runs with ASID 0 and its translations are
 * dropped whenever it is loaded. Protected by asid_lock. */
#define ASID_COUNT 256
static DEFINE_SPINLOCK(asid_lock);
static uintptr_t asid_roots[ASID_COUNT];

static uintptr_t image_phys(const void *p)
{
    return (uintptr_t)p - bootinfo.kernel_virt_base + bootinfo.kernel_phys_base;
}

uintptr_t paging_init_kernel_root(void)
{
    kernel_root = paging_alloc_table();
    return kernel_root;
}

void paging_init_user_root(uintptr_t root, uintptr_t kroot)
{
    (void)kroot;
    spin_lock(&asid_lock);
    for (unsigned i = 1; i < ASID_COUNT; i++) {
        if (!asid_roots[i]) {
            asid_roots[i] = root;
            break;
        }
    }
    spin_unlock(&asid_lock);
}

static unsigned asid_of(uintptr_t root)
{
    unsigned asid = 0;
    spin_lock(&asid_lock);
    for (unsigned i = 1; i < ASID_COUNT; i++) {
        if (asid_roots[i] == root) {
            asid = i;
            break;
        }
    }
    spin_unlock(&asid_lock);
    return asid;
}

void paging_release_user_root(uintptr_t root)
{
    unsigned asid = asid_of(root);
    /* No CPU uses the space any more (vmspace_destroy). Drop the entries
     * of its ASID from the TLBs of every CPU before the ASID is reused. */
    __asm__ volatile("dsb ishst; tlbi aside1is, %0; dsb ish; isb"
                     : : "r"((uint64_t)asid << 48) : "memory");
    if (!asid)
        return;
    spin_lock(&asid_lock);
    asid_roots[asid] = 0;
    spin_unlock(&asid_lock);
}

void paging_load(uintptr_t root)
{
    if (root == kernel_root) {
        uint64_t ttbr1;
        __asm__ volatile("mrs %0, ttbr1_el1" : "=r"(ttbr1));
        if ((ttbr1 & PTE_ADDR_MASK) != root) {
            /* The first activation: the kernel root replaces Limine's
             * tables. The early device mappings move along. */
            early_mmio_install(root);
            __asm__ volatile("dsb ishst; msr ttbr1_el1, %0; isb; tlbi vmalle1; dsb nsh; isb"
                             : : "r"(root) : "memory");
        }
        __asm__ volatile("msr ttbr0_el1, %0; isb" : : "r"(image_phys(empty_root)) : "memory");
        return;
    }
    uint64_t asid = asid_of(root);
    __asm__ volatile("msr ttbr0_el1, %0; isb" : : "r"(root | (asid << 48)) : "memory");
    if (!asid)
        __asm__ volatile("tlbi aside1, xzr; dsb nsh; isb" : : : "memory");
}

void paging_flush_range(uintptr_t root, bool kernel, bool active, uintptr_t va, size_t size)
{
    (void)active;
    /* The table updates are visible to the walkers of every CPU before
     * the invalidation (dsb ishst), and the invalidation is complete on
     * every CPU before the caller continues (dsb ish). */
    __asm__ volatile("dsb ishst" : : : "memory");
    if (kernel) {
        if (size > 64 * PAGE_SIZE) {
            __asm__ volatile("tlbi vmalle1is" : : : "memory");
        } else {
            for (size_t off = 0; off < size; off += PAGE_SIZE)
                __asm__ volatile("tlbi vaae1is, %0"
                                 : : "r"(((va + off) >> PAGE_SHIFT) & ((1UL << 44) - 1)) : "memory");
        }
    } else {
        /* A root without an ASID runs with ASID 0 (asid_of). */
        uint64_t asid = (uint64_t)asid_of(root) << 48;
        if (size > 64 * PAGE_SIZE) {
            __asm__ volatile("tlbi aside1is, %0" : : "r"(asid) : "memory");
        } else {
            for (size_t off = 0; off < size; off += PAGE_SIZE)
                __asm__ volatile("tlbi vae1is, %0"
                                 : : "r"(asid | (((va + off) >> PAGE_SHIFT) & ((1UL << 44) - 1)))
                                 : "memory");
        }
    }
    __asm__ volatile("dsb ish; isb" : : : "memory");
}

void paging_sync_icache(uintptr_t pa)
{
    uint64_t ctr;
    __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
    if (!(ctr & (1UL << 28))) {                 /* IDC: data cleaning not required */
        uintptr_t line = 4UL << ((ctr >> 16) & 0xf);   /* DminLine */
        uintptr_t va = (uintptr_t)P2V(pa & ~(PAGE_SIZE - 1));
        for (uintptr_t p = va; p < va + PAGE_SIZE; p += line)
            __asm__ volatile("dc cvau, %0" : : "r"(p) : "memory");
    }
    __asm__ volatile("dsb ish" : : : "memory");
    if (!(ctr & (1UL << 29)))                   /* DIC: invalidation not required */
        __asm__ volatile("ic ialluis" : : : "memory");
    __asm__ volatile("dsb ish; isb" : : : "memory");
}

/* TCR_EL1 of the boot CPU after paging_enable_features, for the
 * application processors. Written once before they start. */
static uint64_t kernel_tcr;

void paging_enable_features(void)
{
    uint64_t mmfr1, tcr;
    __asm__ volatile("mrs %0, id_aa64mmfr1_el1" : "=r"(mmfr1));
    __asm__ volatile("mrs %0, tcr_el1" : "=r"(tcr));
    unsigned hafdbs = mmfr1 & 0xf;
    hw_access_flag = hafdbs >= 1;
    hw_dirty = hafdbs >= 2;
    /* 8 bit ASIDs (TCR.AS clear) are enough for ASID_COUNT. The hardware
     * access flag and dirty state are enabled where the processor
     * implements them. */
    tcr &= ~TCR_AS;
    if (hw_access_flag)
        tcr |= TCR_HA;
    if (hw_dirty)
        tcr |= TCR_HD;
    __asm__ volatile("msr mair_el1, %0; msr tcr_el1, %1; isb" : : "r"(MAIR_VALUE), "r"(tcr) : "memory");
    kernel_tcr = tcr;
}

void paging_cpu_state(uint64_t *mair, uint64_t *tcr, uint64_t *ttbr1, uint64_t *ttbr0)
{
    *mair = MAIR_VALUE;
    *tcr = kernel_tcr;
    *ttbr1 = kernel_root;
    *ttbr0 = image_phys(empty_root);
}

const char *paging_describe(void)
{
    if (hw_dirty)
        return ", hardware access flag and dirty state";
    if (hw_access_flag)
        return ", hardware access flag";
    return ", software access flag and dirty state";
}
