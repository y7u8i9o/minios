#define KLOG_SUBSYS "paging"
#include <arch/paging.h>
#include <arch/cpu.h>
#include <arch/smp.h>
#include <mm/vmm.h>
#include <boot.h>
#include <sync/spinlock.h>
#include <lib/string.h>
#include <lib/cmdline.h>
#include <lib/printf.h>
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

/* ASIDs (A8). The tag of a space, vmspace.tlb_tag, is its ASID in the low
 * asid_bits bits and the generation in which the ASID was assigned above
 * them. ASIDs are assigned at the first load of a space in a generation
 * and are not released when a space is destroyed. When the generation has
 * no free ASID left, it advances: the bitmap is cleared except for the
 * ASIDs that the CPUs are running, and one broadcast invalidation drops
 * every TLB entry of the old generation. A space retains its ASID across
 * the change when a CPU is running it, and otherwise receives a new one
 * at its next load. ASID 0 is the empty root of the kernel space.
 * asid_lock protects asid_generation, asid_map, asid_next, the tags and
 * the ASID fields of struct arch_cpu. */
#define ASID_BITS_MAX 16
static DEFINE_SPINLOCK(asid_lock);
static unsigned asid_bits = 8;          /* 16 where ID_AA64MMFR0_EL1 reports it, set at boot */
static uint64_t asid_generation;
static uint64_t asid_map[(1UL << ASID_BITS_MAX) / 64];
static uint64_t asid_next = 1;
static uint64_t asid_rollovers;

static uint64_t asid_mask(void)
{
    return (1UL << asid_bits) - 1;
}

static bool asid_used(uint64_t asid)
{
    return asid_map[asid / 64] & (1UL << (asid % 64));
}

static void asid_set(uint64_t asid)
{
    asid_map[asid / 64] |= 1UL << (asid % 64);
}

uint64_t paging_asid_rollovers(void)
{
    return __atomic_load_n(&asid_rollovers, __ATOMIC_RELAXED);
}

/* A new generation. The ASIDs that the CPUs run remain reserved, with the
 * tag of their space, so that the spaces retain them. A CPU that has not
 * switched since an earlier rollover retains its reserved tag, which
 * asid_assign updated when its space was assigned again. */
static void asid_rollover(void)
{
    /* No log line here: paging_load runs under the run queue lock, and
     * logging can take a wait queue lock, which comes first in the lock
     * order. */
    asid_generation++;
    __atomic_store_n(&asid_rollovers, asid_rollovers + 1, __ATOMIC_RELAXED);
    memset(asid_map, 0, sizeof asid_map);
    asid_set(0);
    for (unsigned i = 0; i < smp_cpu_count(); i++) {
        struct cpu *c = cpu_by_id(i);
        if (c->arch.asid_active) {
            c->arch.asid_reserved = c->arch.asid_active;
            c->arch.asid_active = 0;
        }
        asid_set(c->arch.asid_reserved & asid_mask());
    }
    asid_next = 1;
    __asm__ volatile("dsb ishst; tlbi vmalle1is; dsb ish; isb" : : : "memory");
}

/* The tag of vm in the current generation. Called under asid_lock. */
static uint64_t asid_assign(struct vmspace *vm)
{
    uint64_t tag = vm->tlb_tag;
    uint64_t current = asid_generation << asid_bits;
    if (tag && (tag & ~asid_mask()) == current)
        return tag;
    uint64_t old = tag & asid_mask();
    if (tag) {
        /* A space that a CPU ran across the last rollover retains its ASID,
         * and the reservation follows the new tag. */
        bool reserved = false;
        for (unsigned i = 0; i < smp_cpu_count(); i++) {
            struct cpu *c = cpu_by_id(i);
            if (c->arch.asid_reserved == tag) {
                c->arch.asid_reserved = current | old;
                reserved = true;
            }
        }
        if (reserved)
            return vm->tlb_tag = current | old;
        /* An ASID that nobody took in this generation is reused. */
        if (old && !asid_used(old)) {
            asid_set(old);
            return vm->tlb_tag = current | old;
        }
    }
    uint64_t count = 1UL << asid_bits;
    for (uint64_t n = 0; n < count; n++) {
        uint64_t a = (asid_next + n) % count;
        if (a && !asid_used(a)) {
            asid_set(a);
            asid_next = a + 1;
            return vm->tlb_tag = current | a;
        }
    }
    asid_rollover();
    return asid_assign(vm);
}

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
    (void)root;
    (void)kroot;
}

void paging_release_user_root(struct vmspace *vm)
{
    /* No CPU uses the space any more (vmspace_destroy). Its ASID remains
     * assigned until the next rollover, so its TLB entries are dropped
     * now, before its tables are freed. */
    uint64_t asid = vm->tlb_tag & asid_mask();
    if (asid)
        __asm__ volatile("dsb ishst; tlbi aside1is, %0; dsb ish; isb"
                         : : "r"(asid << 48) : "memory");
}

void paging_load(struct vmspace *vm)
{
    if (vm == &kernel_vmspace) {
        uint64_t ttbr1;
        __asm__ volatile("mrs %0, ttbr1_el1" : "=r"(ttbr1));
        if ((ttbr1 & PTE_ADDR_MASK) != kernel_root) {
            /* The first activation: the kernel root replaces Limine's
             * tables. The early device mappings move along. */
            early_mmio_install(kernel_root);
            __asm__ volatile("dsb ishst; msr ttbr1_el1, %0; isb; tlbi vmalle1; dsb nsh; isb"
                             : : "r"(kernel_root) : "memory");
        }
        __asm__ volatile("msr ttbr0_el1, %0; isb" : : "r"(image_phys(empty_root)) : "memory");
        spin_lock(&asid_lock);
        cpu_current()->arch.asid_active = 0;
        cpu_current()->arch.asid_reserved = 0;
        spin_unlock(&asid_lock);
        return;
    }
    spin_lock(&asid_lock);
    uint64_t tag = asid_assign(vm);
    cpu_current()->arch.asid_active = tag;
    cpu_current()->arch.asid_reserved = 0;
    __asm__ volatile("msr ttbr0_el1, %0; isb"
                     : : "r"(vm->pt_root | ((tag & asid_mask()) << 48)) : "memory");
    spin_unlock(&asid_lock);
}

void paging_flush_range(struct vmspace *vm, bool kernel, bool active, uintptr_t va, size_t size)
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
        /* A tag of an older generation names an ASID whose entries the
         * rollover dropped, or that the CPUs still run (reserved); in
         * both cases invalidating that ASID is correct, at worst for
         * another space as well. */
        uint64_t asid = (__atomic_load_n(&vm->tlb_tag, __ATOMIC_RELAXED) & asid_mask()) << 48;
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
    /* 16 bit ASIDs where ID_AA64MMFR0_EL1.ASIDBits reports them, 8 bits
     * otherwise. The hardware access flag and dirty state are enabled
     * where the processor implements them. */
    uint64_t mmfr0;
    __asm__ volatile("mrs %0, id_aa64mmfr0_el1" : "=r"(mmfr0));
    tcr &= ~TCR_AS;
    if (((mmfr0 >> 4) & 0xf) == 2) {
        tcr |= TCR_AS;
        asid_bits = 16;
    }
    /* asid_bits=N on the command line uses fewer bits, so that a test can
     * cause rollovers with a few spaces. */
    char val[8];
    if (cmdline_lookup("asid_bits", val, sizeof val) && val[0] >= '1' && val[0] <= '9') {
        unsigned bits = (unsigned)(val[0] - '0');
        if (val[1] >= '0' && val[1] <= '9')
            bits = bits * 10 + (unsigned)(val[1] - '0');
        if (bits >= 2 && bits < asid_bits)
            asid_bits = bits;
    }
    asid_generation = 1;
    asid_set(0);
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
    static char text[96];
    ksnprintf(text, sizeof text, ", %u bit ASIDs, %s", asid_bits,
              hw_dirty ? "hardware access flag and dirty state" :
              hw_access_flag ? "hardware access flag" : "software access flag and dirty state");
    return text;
}
