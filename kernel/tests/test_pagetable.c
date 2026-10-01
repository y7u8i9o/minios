#include <tests/ktest.h>
#include <arch/paging.h>
#include <mm/ptwalk.h>
#include <mm/vmm.h>
#include <mm/vma.h>
#include <console.h>

/* A2: the page table entry interface and the range walker
 * (docs/design/arch.md). */

static void check_entries(void)
{
    uintptr_t pa = 0x12345000;
    pte_t e = pte_make(pa, VM_READ | VM_WRITE | VM_USER);
    ktest_assert(pte_present(e) && pte_mapped(e) && pte_addr(e) == pa, "page entry %lx", e);
    ktest_assert(pte_write(e) && pte_user(e) && !pte_cow(e) && !pte_swapped(e), "page entry bits %lx", e);
    ktest_assert(pte_vm_flags(e) == (VM_READ | VM_WRITE | VM_USER), "vm flags %x", pte_vm_flags(e));
    ktest_assert(pte_vm_flags(pte_make(pa, VM_READ | VM_EXEC | VM_GLOBAL)) == (VM_READ | VM_EXEC | VM_GLOBAL),
                 "kernel text flags");
    ktest_assert(pte_vm_flags(pte_make(pa, VM_KERNEL_RW | VM_WC)) == (VM_KERNEL_RW | VM_WC), "write combining");
    ktest_assert(pte_vm_flags(pte_make(pa, VM_KERNEL_RW | VM_NOCACHE)) == (VM_KERNEL_RW | VM_NOCACHE), "uncached");

    pte_t c = pte_mkcow(pte_wrprotect(e));
    ktest_assert(pte_cow(c) && !pte_write(c) && pte_addr(c) == pa, "copy on write entry %lx", c);
    pte_t w = pte_mkwrite(pte_clear_cow(c));
    ktest_assert(w == e, "copy on write resolved to %lx, expected %lx", w, e);
    ktest_assert(pte_addr(pte_set_addr(c, 0x777000)) == 0x777000 && pte_cow(pte_set_addr(c, 0x777000)),
                 "pte_set_addr lost the attributes");

    pte_t y = pte_mkdirty(pte_mkyoung(e));
    ktest_assert(pte_young(y) && pte_dirty(y) && !pte_young(pte_mkold(y)) && !pte_dirty(pte_mkclean(y)),
                 "accessed and dirty bits");
    ktest_assert(pte_lazyfree(pte_mklazyfree(e)) && pte_clear_lazyfree(pte_mklazyfree(e)) == e, "lazy free");

    pte_t n = pte_make_protnone(pa);
    ktest_assert(!pte_present(n) && pte_mapped(n) && pte_protnone(n) && pte_addr(n) == pa, "PROT_NONE %lx", n);
    ktest_assert(vma_make_pte(pa, VM_WRITE) == n, "unreadable region did not give PROT_NONE");

    pte_t s = pte_make_swap(12345);
    ktest_assert(!pte_present(s) && !pte_mapped(s) && pte_swapped(s) && pte_swap_slot(s) == 12345,
                 "swap entry %lx", s);

    pte_t b = pte_mkblock(pte_make(0x40000000, VM_READ | VM_USER));
    ktest_assert(pte_is_block(b) && !pte_is_table(b) && pte_block_to_page(b) == pte_make(0x40000000, VM_READ | VM_USER),
                 "block entry %lx", b);
    pte_t t = pte_make_table(0x9000, true);
    ktest_assert(pte_is_table(t) && !pte_is_block(t) && pte_addr(t) == 0x9000, "table entry %lx", t);
}

/* Three pages in different level 4, level 3 and level 2 ranges: the walk
 * must return exactly their three level 1 tables, in address order. */
static void check_walker(void)
{
    struct vmspace *vm = vmspace_create();
    ktest_assert(vm != NULL, "address space");
    uintptr_t pages[3] = { 0x400000, 0x40000000 + 0x200000, 1UL << 40 };
    unsigned flags = VM_READ | VM_WRITE | VM_USER | VM_MMAP;
    for (int i = 0; i < 3; i++)
        ktest_assert(vma_add(vm, pages[i], pages[i] + PAGE_SIZE, flags) == 0 &&
                     vma_populate(vm, pages[i], pages[i] + PAGE_SIZE) == 0, "populate %lx", pages[i]);
    spin_lock(&vm->lock);
    uintptr_t va = USER_BASE;
    pte_t *pde, *pt;
    int found = 0;
    while ((pt = pt_next_leaf_table(vm->pt_root, &va, USER_TOP + 1, &pde)) != NULL) {
        ktest_assert(found < 3, "walk returned more than three tables");
        ktest_assert(ALIGN_DOWN(va, PAGE_2M) == ALIGN_DOWN(pages[found], PAGE_2M),
                     "table %d at %lx, expected %lx", found, va, pages[found]);
        ktest_assert(pte_present(pt[PT_INDEX(pages[found], 1)]) && pte_table(*pde) == pt,
                     "table %d does not map %lx", found, pages[found]);
        found++;
        va = ALIGN_DOWN(va, PAGE_2M) + PAGE_2M;
    }
    spin_unlock(&vm->lock);
    ktest_assert(found == 3 && va >= USER_TOP + 1, "walk found %d tables and stopped at %lx", found, va);
    vma_remove_all(vm);
    vmspace_destroy(vm);
}

static void test_pagetable(void)
{
    check_entries();
    check_walker();
    kprintf("pagetable: %d levels of %d entries, entries and walk checked\n", PT_LEVELS, PT_ENTRIES);
}
KTEST_DEFINE_STAGE("pagetable", test_pagetable, KTEST_MEMORY);
