#include <arch/paging.h>
#include <mm/vmm.h>
#include "todo.h"

/* Entry construction for the VM_* flags. The table operations follow with
 * the MMU code of A5. */

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

uintptr_t paging_alloc_table(void) { ARCH_TODO("A5"); }
void paging_free_table(uintptr_t pa) { ARCH_TODO("A5"); }
int paging_walk(uintptr_t root, uintptr_t va, bool create, pte_t **entry) { ARCH_TODO("A5"); }
int paging_walk_preallocated(uintptr_t root, uintptr_t va, const uintptr_t *tables, unsigned table_count,
                             unsigned *used, pte_t **entry) { ARCH_TODO("A5"); }
int paging_pde(uintptr_t root, uintptr_t va, bool create, pte_t **entry) { ARCH_TODO("A5"); }
int paging_map_large(uintptr_t root, uintptr_t va, uintptr_t pa, size_t size, unsigned vm_flags) { ARCH_TODO("A5"); }
uintptr_t paging_init_kernel_root(void) { ARCH_TODO("A5"); }
void paging_init_user_root(uintptr_t root, uintptr_t kernel_root) { ARCH_TODO("A5"); }
void paging_free_user_tables(uintptr_t root) { ARCH_TODO("A5"); }
void paging_load(uintptr_t root) { ARCH_TODO("A5"); }
void paging_flush_page(uintptr_t va) { ARCH_TODO("A5"); }
void paging_flush_user(void) { ARCH_TODO("A5"); }
void paging_enable_features(void) { ARCH_TODO("A5"); }

const char *paging_describe(void)
{
    return "";
}
