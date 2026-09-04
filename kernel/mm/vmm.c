#define KLOG_SUBSYS "vmm"
#include <mm/vmm.h>
#include <mm/pmm.h>
#include <mm/slab.h>
#include <mm/swap.h>
#include <mm/memlayout.h>
#include <arch/paging.h>
#include <arch/boot.h>
#include <arch/cpu.h>
#include <arch/trap.h>
#include <lib/string.h>
#include <kassert.h>
#include <klog.h>
#include <errno.h>
#include <debug/panic.h>

/* The kernel address space. Its lock, kvm_lock, serializes every change to
 * the shared upper half page tables. */
struct vmspace kernel_vmspace = { .pml4_phys = 0, .lock = SPINLOCK_INIT("kvm_lock") };
LIST_HEAD(vmspaces);
DEFINE_SPINLOCK(vmspaces_lock);

extern char __rodata_start[], __rodata_end[], __data_start[], __data_end[];
extern char boot_stack[];

/* Kernel stack slots and MMIO bump pointer. Protected by kvm_lock. */
static uint64_t kstack_bitmap[KSTACK_SLOTS / 64];
static uintptr_t kmmio_next = KMMIO_BASE;

static uint64_t flags_to_pte(unsigned flags)
{
    uint64_t pte = PTE_P;
    if (flags & VM_WRITE)
        pte |= PTE_W;
    if (flags & VM_USER)
        pte |= PTE_U;
    if (!(flags & VM_EXEC))
        pte |= PTE_NX;
    if (flags & VM_NOCACHE)
        pte |= PTE_PCD | PTE_PWT;
    else if (flags & VM_WC)
        pte |= PTE_PWT;
    if (flags & VM_GLOBAL)
        pte |= PTE_G;
    return pte;
}

static unsigned pte_to_flags(uint64_t pte)
{
    unsigned flags = VM_READ;
    if (pte & PTE_W)
        flags |= VM_WRITE;
    if (pte & PTE_U)
        flags |= VM_USER;
    if (!(pte & PTE_NX))
        flags |= VM_EXEC;
    if ((pte & (PTE_PCD | PTE_PWT)) == (PTE_PCD | PTE_PWT))
        flags |= VM_NOCACHE;
    else if (pte & PTE_PWT)
        flags |= VM_WC;
    if (pte & PTE_G)
        flags |= VM_GLOBAL;
    return flags;
}

static int map_locked(struct vmspace *vm, uintptr_t va, uintptr_t pa, size_t size, unsigned flags)
{
    uint64_t pte_flags = flags_to_pte(flags);
    for (size_t off = 0; off < size; off += PAGE_SIZE) {
        uint64_t *entry;
        int r = paging_walk(vm->pml4_phys, va + off, true, &entry);
        if (r < 0)
            return r;
        if (r != 1)
            return -EEXIST;
        if (*entry & PTE_P)
            return -EEXIST;
        *entry = (pa + off) | pte_flags;
    }
    return 0;
}

int vmm_map(struct vmspace *vm, uintptr_t va, uintptr_t pa, size_t size, unsigned flags)
{
    if (!IS_ALIGNED(va, PAGE_SIZE) || !IS_ALIGNED(pa, PAGE_SIZE) || !IS_ALIGNED(size, PAGE_SIZE))
        return -EINVAL;
    if (vm != &kernel_vmspace && (va > USER_TOP || va + size - 1 > USER_TOP))
        return -EINVAL;
    spin_lock(&vm->lock);
    int r = map_locked(vm, va, pa, size, flags);
    spin_unlock(&vm->lock);
    return r;
}

int vmm_unmap(struct vmspace *vm, uintptr_t va, size_t size)
{
    if (!IS_ALIGNED(va, PAGE_SIZE) || !IS_ALIGNED(size, PAGE_SIZE))
        return -EINVAL;
    spin_lock(&vm->lock);
    for (size_t off = 0; off < size; off += PAGE_SIZE) {
        uint64_t *entry;
        int r = paging_walk(vm->pml4_phys, va + off, false, &entry);
        if (r == 1)
            *entry = 0;
        else if (r == 2)
            panic("vmm_unmap: %lx is inside a 2 MiB mapping", va + off);
    }
    tlb_flush_range(vm, va, size);
    spin_unlock(&vm->lock);
    return 0;
}

int vmm_protect(struct vmspace *vm, uintptr_t va, size_t size, unsigned flags)
{
    if (!IS_ALIGNED(va, PAGE_SIZE) || !IS_ALIGNED(size, PAGE_SIZE))
        return -EINVAL;
    uint64_t pte_flags = flags_to_pte(flags);
    spin_lock(&vm->lock);
    for (size_t off = 0; off < size; off += PAGE_SIZE) {
        uint64_t *entry;
        int r = paging_walk(vm->pml4_phys, va + off, false, &entry);
        if (r != 1 || !(*entry & PTE_P)) {
            spin_unlock(&vm->lock);
            return -ENOENT;
        }
        *entry = (*entry & PTE_ADDR_MASK) | pte_flags;
    }
    tlb_flush_range(vm, va, size);
    spin_unlock(&vm->lock);
    return 0;
}

bool vmm_translate(struct vmspace *vm, uintptr_t va, uintptr_t *pa, unsigned *flags)
{
    uint64_t *entry;
    spin_lock(&vm->lock);
    int r = paging_walk(vm->pml4_phys, va, false, &entry);
    bool ok = r > 0 && (*entry & PTE_P);
    if (ok) {
        uintptr_t mask = r == 2 ? PAGE_2M - 1 : PAGE_SIZE - 1;
        if (pa)
            *pa = (*entry & PTE_ADDR_MASK & ~mask) | (va & mask);
        if (flags)
            *flags = pte_to_flags(*entry);
    }
    spin_unlock(&vm->lock);
    return ok;
}

struct vmspace *vmspace_create(void)
{
    struct vmspace *vm = kzalloc(sizeof *vm);
    if (!vm)
        return NULL;
    spinlock_init(&vm->lock, "vmspace");
    list_init(&vm->vmas);
    vm->pml4_phys = paging_alloc_table();
    if (!vm->pml4_phys) {
        kfree(vm);
        return NULL;
    }
    /* Share the kernel half. Every upper PML4 entry was populated at
     * vmm_init, so later kernel mappings never need to touch user spaces. */
    uint64_t *src = P2V(kernel_vmspace.pml4_phys);
    uint64_t *dst = P2V(vm->pml4_phys);
    memcpy(&dst[PT_ENTRIES / 2], &src[PT_ENTRIES / 2], PAGE_SIZE / 2);
    spin_lock(&vmspaces_lock);
    list_add_tail(&vm->link, &vmspaces);
    spin_unlock(&vmspaces_lock);
    return vm;
}

static void free_user_level(uint64_t *table, int level)
{
    for (int i = 0; i < PT_ENTRIES; i++) {
        uint64_t e = table[i];
        if (level == 1 && (e & PTE_SWAPPED)) {
            swap_free_slot(e >> 12);
            table[i] = 0;
            continue;
        }
        if (level == 1 && (e & PTE_PROTNONE)) {
            if (pmm_is_ram(e & PTE_ADDR_MASK))
                page_put(phys_to_page(e & PTE_ADDR_MASK));
            table[i] = 0;
            continue;
        }
        if (!(e & PTE_P))
            continue;
        if (level == 1) {
            if (pmm_is_ram(e & PTE_ADDR_MASK))
                page_put(phys_to_page(e & PTE_ADDR_MASK));
            table[i] = 0;
        } else if (!(e & PTE_PS)) {
            free_user_level(P2V(e & PTE_ADDR_MASK), level - 1);
        }
    }
}

void vmspace_free_user_pages(struct vmspace *vm)
{
    kassert(vm != &kernel_vmspace);
    spin_lock(&vm->lock);
    uint64_t *pml4 = P2V(vm->pml4_phys);
    for (int i = 0; i < PT_ENTRIES / 2; i++) {
        if (pml4[i] & PTE_P)
            free_user_level(P2V(pml4[i] & PTE_ADDR_MASK), 3);
    }
    tlb_flush_range(vm, 0, USER_TOP + 1);
    spin_unlock(&vm->lock);
}

void vmspace_destroy(struct vmspace *vm)
{
    kassert(vm != &kernel_vmspace);
    kassert(list_empty(&vm->vmas));
    spin_lock(&vmspaces_lock);
    list_del(&vm->link);
    spin_unlock(&vmspaces_lock);
    /* Kernel threads switch address spaces lazily, so the space being
     * destroyed may still be loaded here or on another CPU. */
    if (cpu_current()->vm == vm)
        vmspace_activate(&kernel_vmspace);
    tlb_drop_vmspace(vm);
    kassert(vm->cpu_mask == 0);
    paging_free_user_tables(vm->pml4_phys);
    kfree(vm);
}

void vmspace_activate(struct vmspace *vm)
{
    struct cpu *c = cpu_current();
    cpu_mask_t bit = 1UL << c->id;
    struct vmspace *old = c->vm;
    if (old == vm)
        return;
    /* The mask of the new space is set before CR3 changes and the old one
     * is cleared afterwards, so a shootdown never misses a CPU that holds
     * translations of either space. */
    __atomic_fetch_or(&vm->cpu_mask, bit, __ATOMIC_SEQ_CST);
    c->vm = vm;
    paging_load(vm->pml4_phys);
    if (old)
        __atomic_fetch_and(&old->cpu_mask, ~bit, __ATOMIC_SEQ_CST);
}

struct vmspace *vmspace_current(void)
{
    return cpu_current()->vm;
}

void *vmm_map_mmio(uintptr_t pa, size_t size, unsigned flags)
{
    uintptr_t off = pa & (PAGE_SIZE - 1);
    uintptr_t base = pa - off;
    size = ALIGN_UP(size + off, PAGE_SIZE);
    spin_lock(&kernel_vmspace.lock);
    uintptr_t va = kmmio_next;
    if (va + size > KMMIO_BASE + KMMIO_SIZE) {
        spin_unlock(&kernel_vmspace.lock);
        return NULL;
    }
    kmmio_next += size;
    int r = map_locked(&kernel_vmspace, va, base, size, flags | VM_GLOBAL);
    spin_unlock(&kernel_vmspace.lock);
    if (r < 0)
        return NULL;
    return (void *)(va + off);
}

void *kstack_alloc(void)
{
    spin_lock(&kernel_vmspace.lock);
    int slot = -1;
    for (int i = 0; i < KSTACK_SLOTS / 64 && slot < 0; i++) {
        if (kstack_bitmap[i] == ~0UL)
            continue;
        for (int b = 0; b < 64; b++) {
            if (!(kstack_bitmap[i] & (1UL << b))) {
                kstack_bitmap[i] |= 1UL << b;
                slot = i * 64 + b;
                break;
            }
        }
    }
    if (slot < 0) {
        spin_unlock(&kernel_vmspace.lock);
        return NULL;
    }
    /* Slot layout: one guard page, then KSTACK_SIZE of mapped stack. */
    uintptr_t base = KSTACK_BASE + (uintptr_t)slot * KSTACK_SLOT + PAGE_SIZE;
    for (size_t off = 0; off < KSTACK_SIZE; off += PAGE_SIZE) {
        struct page *pg = pmm_alloc_page();
        if (!pg || map_locked(&kernel_vmspace, base + off, page_to_phys(pg), PAGE_SIZE, VM_KERNEL_RW) < 0) {
            spin_unlock(&kernel_vmspace.lock);
            kstack_free((void *)(base + off));
            return NULL;
        }
    }
    spin_unlock(&kernel_vmspace.lock);
    return (void *)(base + KSTACK_SIZE);
}

void kstack_free(void *top)
{
    uintptr_t t = (uintptr_t)top;
    kassert(t > KSTACK_BASE && t <= KSTACK_BASE + (uintptr_t)KSTACK_SLOTS * KSTACK_SLOT);
    int slot = (int)((t - KSTACK_BASE - 1) / KSTACK_SLOT);
    uintptr_t base = KSTACK_BASE + (uintptr_t)slot * KSTACK_SLOT + PAGE_SIZE;
    spin_lock(&kernel_vmspace.lock);
    for (uintptr_t va = base; va < base + KSTACK_SIZE; va += PAGE_SIZE) {
        uint64_t *entry;
        if (paging_walk(kernel_vmspace.pml4_phys, va, false, &entry) == 1 && (*entry & PTE_P)) {
            pmm_free_page(phys_to_page(*entry & PTE_ADDR_MASK));
            *entry = 0;
        }
    }
    tlb_flush_range(&kernel_vmspace, base, KSTACK_SIZE);
    kstack_bitmap[slot / 64] &= ~(1UL << (slot % 64));
    spin_unlock(&kernel_vmspace.lock);
}

static void map_kernel_image(uintptr_t pml4)
{
    uintptr_t phys = bootinfo.kernel_phys_base;
    uintptr_t virt = bootinfo.kernel_virt_base;
    struct {
        char *start, *end;
        unsigned flags;
    } segs[] = {
        { __text_start, __rodata_start, VM_READ | VM_EXEC | VM_GLOBAL },
        { __rodata_start, __data_start, VM_READ | VM_GLOBAL },
        { __data_start, __kernel_end, VM_KERNEL_RW },
    };
    for (size_t i = 0; i < ARRAY_SIZE(segs); i++) {
        uintptr_t s = ALIGN_DOWN((uintptr_t)segs[i].start, PAGE_SIZE);
        uintptr_t e = ALIGN_UP((uintptr_t)segs[i].end, PAGE_SIZE);
        if (e <= s)
            continue;
        if (paging_map_large(pml4, s, s - virt + phys, e - s, flags_to_pte(segs[i].flags)) < 0)
            panic("vmm: cannot map kernel image");
    }
}

static void map_hhdm(uintptr_t pml4)
{
    for (size_t i = 0; i < bootinfo.memmap_count; i++) {
        const struct limine_memmap_entry *e = &bootinfo.memmap[i];
        unsigned flags;
        switch (e->type) {
        case LIMINE_MEMMAP_USABLE:
        case LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE:
        case LIMINE_MEMMAP_EXECUTABLE_AND_MODULES:
        case LIMINE_MEMMAP_ACPI_RECLAIMABLE:
        case LIMINE_MEMMAP_ACPI_NVS:
            flags = VM_KERNEL_RW;
            break;
        case LIMINE_MEMMAP_FRAMEBUFFER:
            flags = VM_KERNEL_RW | VM_WC;
            break;
        default:
            continue;
        }
        uintptr_t s = ALIGN_DOWN(e->base, PAGE_SIZE);
        uintptr_t end = ALIGN_UP(e->base + e->length, PAGE_SIZE);
        if (paging_map_large(pml4, s + hhdm_offset, s, end - s, flags_to_pte(flags)) < 0)
            panic("vmm: cannot map hhdm");
    }
}

void vmm_init(void)
{
    uintptr_t pml4 = paging_alloc_table();
    if (!pml4)
        panic("vmm: no memory for PML4");

    /* Populate every upper half PML4 entry now so user spaces can copy
     * them once and stay in sync with all later kernel mappings. */
    uint64_t *table = P2V(pml4);
    for (int i = PT_ENTRIES / 2; i < PT_ENTRIES; i++) {
        uintptr_t pdpt = paging_alloc_table();
        if (!pdpt)
            panic("vmm: no memory for kernel PDPTs");
        table[i] = pdpt | PTE_P | PTE_W;
    }

    map_hhdm(pml4);
    map_kernel_image(pml4);

    kernel_vmspace.pml4_phys = pml4;
    paging_enable_features();
    vmspace_activate(&kernel_vmspace);

    /* The boot stack is in .bss. Its first page becomes the guard page. */
    uintptr_t guard = (uintptr_t)boot_stack;
    kassert(IS_ALIGNED(guard, PAGE_SIZE));
    vmm_unmap(&kernel_vmspace, guard, PAGE_SIZE);

    klog_info("kernel page tables active, pml4 at %lx", pml4);
}
