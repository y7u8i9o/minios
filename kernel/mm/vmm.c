#define KLOG_SUBSYS "vmm"
#include <mm/vmm.h>
#include <mm/pmm.h>
#include <mm/slab.h>
#include <mm/swap.h>
#include <mm/huge.h>
#include <mm/memlayout.h>
#include <arch/paging.h>
#include <boot.h>
#include <arch/cpu.h>
#include <lib/string.h>
#include <kassert.h>
#include <klog.h>
#include <errno.h>
#include <debug/panic.h>

/* The kernel address space. Its lock, kvm_lock, serializes every change to
 * the shared upper half page tables. */
struct vmspace kernel_vmspace = { .pt_root = 0, .lock = SPINLOCK_INIT("kvm_lock") };
LIST_HEAD(vmspaces);
DEFINE_SPINLOCK(vmspaces_lock);

extern char __rodata_start[], __rodata_end[], __data_start[], __data_end[];
extern char boot_stack[];

/* Kernel stack slots and MMIO bump pointer. Protected by kvm_lock. */
static uint64_t kstack_bitmap[KSTACK_SLOTS / 64];
static uintptr_t kmmio_next = KMMIO_BASE;

static int map_locked(struct vmspace *vm, uintptr_t va, uintptr_t pa, size_t size, unsigned flags)
{
    for (size_t off = 0; off < size; off += PAGE_SIZE) {
        pte_t *entry;
        int r = paging_walk(vm->pt_root, va + off, true, &entry);
        if (r < 0)
            return r;
        if (r != 1)
            return -EEXIST;
        if (pte_present(*entry))
            return -EEXIST;
        *entry = pte_make(pa + off, flags);
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
    if (r == 0 && vm != &kernel_vmspace)
        for (size_t off = 0; off < size; off += PAGE_SIZE)
            if (pmm_is_ram(pa + off))
                percpu_counter_inc(&vm->resident);
    spin_unlock(&vm->lock);
    return r;
}

int vmm_unmap(struct vmspace *vm, uintptr_t va, size_t size)
{
    if (!IS_ALIGNED(va, PAGE_SIZE) || !IS_ALIGNED(size, PAGE_SIZE))
        return -EINVAL;
    spin_lock(&vm->lock);
    for (size_t off = 0; off < size; off += PAGE_SIZE) {
        pte_t *entry;
        int r = paging_walk(vm->pt_root, va + off, false, &entry);
        if (r == 1) {
            if (vm != &kernel_vmspace && pte_present(*entry) && pmm_is_ram(pte_addr(*entry)))
                percpu_counter_dec(&vm->resident);
            *entry = 0;
        }
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
    spin_lock(&vm->lock);
    for (size_t off = 0; off < size; off += PAGE_SIZE) {
        pte_t *entry;
        int r = paging_walk(vm->pt_root, va + off, false, &entry);
        if (r != 1 || !pte_present(*entry)) {
            spin_unlock(&vm->lock);
            return -ENOENT;
        }
        *entry = pte_make(pte_addr(*entry), flags);
    }
    tlb_flush_range(vm, va, size);
    spin_unlock(&vm->lock);
    return 0;
}

bool vmm_translate(struct vmspace *vm, uintptr_t va, uintptr_t *pa, unsigned *flags)
{
    pte_t *entry;
    spin_lock(&vm->lock);
    int r = paging_walk(vm->pt_root, va, false, &entry);
    bool ok = r > 0 && pte_present(*entry);
    if (ok) {
        uintptr_t mask = r == 2 ? PAGE_2M - 1 : PAGE_SIZE - 1;
        if (pa)
            *pa = (pte_addr(*entry) & ~mask) | (va & mask);
        if (flags)
            *flags = pte_vm_flags(*entry);
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
    percpu_counter_init(&vm->resident, 0);
    vm->pt_root = paging_alloc_table();
    if (!vm->pt_root) {
        kfree(vm);
        return NULL;
    }
    paging_init_user_root(vm->pt_root, kernel_vmspace.pt_root);
    spin_lock(&vmspaces_lock);
    list_add_tail(&vm->link, &vmspaces);
    spin_unlock(&vmspaces_lock);
    return vm;
}

static void free_user_level(pte_t *table, int level)
{
    for (int i = 0; i < PT_ENTRIES; i++) {
        pte_t e = table[i];
        if (level == 1 && pte_swapped(e)) {
            swap_free_slot(pte_swap_slot(e));
            table[i] = 0;
            continue;
        }
        if (level == 1 && pte_protnone(e)) {
            if (pmm_is_ram(pte_addr(e)))
                page_put(phys_to_page(pte_addr(e)));
            table[i] = 0;
            continue;
        }
        if (!pte_present(e))
            continue;
        if (level == 1) {
            if (pmm_is_ram(pte_addr(e)))
                page_put(phys_to_page(pte_addr(e)));
            table[i] = 0;
        } else if (level == 2 && pte_is_block(e)) {
            huge_unmap_locked(&table[i]);
        } else if (pte_is_table(e)) {
            free_user_level(pte_table(e), level - 1);
        }
    }
}

void vmspace_free_user_pages(struct vmspace *vm)
{
    kassert(vm != &kernel_vmspace);
    spin_lock(&vm->lock);
    pte_t *root = P2V(vm->pt_root);
    for (int i = 0; i < PT_ROOT_USER_ENTRIES; i++) {
        if (pte_is_table(root[i]))
            free_user_level(pte_table(root[i]), PT_LEVELS - 1);
    }
    tlb_flush_range(vm, 0, USER_TOP + 1);
    percpu_counter_init(&vm->resident, 0);
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
    paging_free_user_tables(vm->pt_root);
    kfree(vm);
}

void vmspace_activate(struct vmspace *vm)
{
    struct cpu *c = cpu_current();
    cpu_mask_t bit = 1UL << c->id;
    struct vmspace *old = c->vm;
    if (old == vm)
        return;
    /* The mask of the new space is set before the root changes and the old one
     * is cleared afterwards, so a shootdown never misses a CPU that holds
     * translations of either space. */
    __atomic_fetch_or(&vm->cpu_mask, bit, __ATOMIC_SEQ_CST);
    c->vm = vm;
    paging_load(vm->pt_root);
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
        pte_t *entry;
        if (paging_walk(kernel_vmspace.pt_root, va, false, &entry) == 1 && pte_present(*entry)) {
            pmm_free_page(phys_to_page(pte_addr(*entry)));
            *entry = 0;
        }
    }
    tlb_flush_range(&kernel_vmspace, base, KSTACK_SIZE);
    kstack_bitmap[slot / 64] &= ~(1UL << (slot % 64));
    spin_unlock(&kernel_vmspace.lock);
}

static void map_kernel_image(uintptr_t root)
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
        if (paging_map_large(root, s, s - virt + phys, e - s, segs[i].flags) < 0)
            panic("vmm: cannot map kernel image");
    }
}

static uint64_t hhdm_mapped_bytes;   /* written once by map_hhdm */

static void map_hhdm(uintptr_t root)
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
        if (paging_map_large(root, s + hhdm_offset, s, end - s, flags) < 0)
            panic("vmm: cannot map hhdm");
        hhdm_mapped_bytes += end - s;
    }
}

void vmm_init(void)
{
    uintptr_t root = paging_init_kernel_root();
    if (!root)
        panic("vmm: no memory for the kernel page tables");

    map_hhdm(root);
    map_kernel_image(root);

    kernel_vmspace.pt_root = root;
    paging_enable_features();
    vmspace_activate(&kernel_vmspace);

    /* The boot stack is in .bss. Its first page becomes the guard page. */
    uintptr_t guard = (uintptr_t)boot_stack;
    kassert(IS_ALIGNED(guard, PAGE_SIZE));
    vmm_unmap(&kernel_vmspace, guard, PAGE_SIZE);

    klog_info("kernel page tables active, root table at %lx, %lu MiB of physical memory in the hhdm%s",
              root, hhdm_mapped_bytes >> 20, paging_describe());
    klog_info("image text %lu KiB, rodata %lu KiB, data and bss %lu KiB, boot stack guard at %lx",
              (unsigned long)(__rodata_start - __text_start) >> 10,
              (unsigned long)(__data_start - __rodata_start) >> 10,
              (unsigned long)(__kernel_end - __data_start) >> 10, guard);
}
