#include <arch/paging.h>
#include <mm/pmm.h>
#include <mm/memlayout.h>
#include <mm/vmm.h>
#include <lib/string.h>
#include <errno.h>

/* The table operations of <arch/paging.h> that only walk and allocate
 * tables (A5). They use the geometry and the entry functions of the
 * architecture, so x86_64 and aarch64 share them. The entry format, the
 * roots and the TLB are architecture code. */

uintptr_t paging_alloc_table(void)
{
    struct page *pg = pmm_alloc_page();
    if (!pg)
        return 0;
    uintptr_t pa = page_to_phys(pg);
    memset(P2V(pa), 0, PAGE_SIZE);
    return pa;
}

void paging_free_table(uintptr_t pa)
{
    pmm_free_page(phys_to_page(pa));
}

/* Descend one level, creating the next table if asked. A block entry ends
 * the walk. */
static pte_t *next_level(pte_t *entry, bool create, bool user)
{
    if (pte_present(*entry))
        return pte_is_table(*entry) ? pte_table(*entry) : NULL;
    if (!create)
        return NULL;
    uintptr_t pa = paging_alloc_table();
    if (!pa)
        return NULL;
    paging_publish_entries();           /* the cleared table before the link */
    *entry = pte_make_table(pa, user);
    return P2V(pa);
}

/* The same with tables allocated by the caller. */
static pte_t *next_level_preallocated(pte_t *entry, bool user, const uintptr_t *tables,
                                      unsigned table_count, unsigned *used)
{
    if (pte_present(*entry))
        return pte_is_table(*entry) ? pte_table(*entry) : NULL;
    if (*used >= table_count)
        return NULL;
    uintptr_t pa = tables[(*used)++];
    paging_publish_entries();
    *entry = pte_make_table(pa, user);
    return P2V(pa);
}

int paging_pde(uintptr_t root, uintptr_t va, bool create, pte_t **entry)
{
    bool user = va <= USER_TOP;
    pte_t *table = P2V(root);
    for (int level = PT_LEVELS; level > 2; level--) {
        table = next_level(&table[PT_INDEX(va, level)], create, user);
        if (!table)
            return create ? -ENOMEM : 0;
    }
    *entry = &table[PT_INDEX(va, 2)];
    return 1;
}

int paging_walk(uintptr_t root, uintptr_t va, bool create, pte_t **entry)
{
    pte_t *pde;
    int r = paging_pde(root, va, create, &pde);
    if (r <= 0)
        return r;
    if (pte_is_block(*pde)) {
        *entry = pde;
        return 2;
    }
    pte_t *pt = next_level(pde, create, va <= USER_TOP);
    if (!pt)
        return create ? -ENOMEM : 0;
    *entry = &pt[PT_INDEX(va, 1)];
    return 1;
}

int paging_walk_preallocated(uintptr_t root, uintptr_t va,
                             const uintptr_t *tables, unsigned table_count,
                             unsigned *used, pte_t **entry)
{
    bool user = va <= USER_TOP;
    pte_t *table = P2V(root);
    *used = 0;
    for (int level = PT_LEVELS; level > 2; level--) {
        table = next_level_preallocated(&table[PT_INDEX(va, level)], user, tables, table_count, used);
        if (!table)
            return -ENOMEM;
    }
    pte_t *pde = &table[PT_INDEX(va, 2)];
    if (pte_is_block(*pde)) {
        *entry = pde;
        return 2;
    }
    pte_t *pt = next_level_preallocated(pde, user, tables, table_count, used);
    if (!pt)
        return -ENOMEM;
    *entry = &pt[PT_INDEX(va, 1)];
    return 1;
}

int paging_map_large(uintptr_t root, uintptr_t va, uintptr_t pa, size_t size, unsigned vm_flags)
{
    uintptr_t end = va + size;
    while (va < end) {
        pte_t *entry;
        bool big = IS_ALIGNED(va, PAGE_2M) && IS_ALIGNED(pa, PAGE_2M) && end - va >= PAGE_2M;
        if (big) {
            int r = paging_pde(root, va, true, &entry);
            if (r < 0)
                return r;
            *entry = pte_mkblock(pte_make(pa, vm_flags));
            va += PAGE_2M;
            pa += PAGE_2M;
            continue;
        }
        int r = paging_walk(root, va, true, &entry);
        if (r < 0)
            return r;
        *entry = pte_make(pa, vm_flags);
        va += PAGE_SIZE;
        pa += PAGE_SIZE;
    }
    paging_publish_entries();
    return 0;
}

static void free_level(pte_t *table, int level)
{
    for (int i = 0; i < PT_ENTRIES; i++) {
        pte_t e = table[i];
        if (!pte_is_table(e))
            continue;
        if (level > 1)
            free_level(pte_table(e), level - 1);
        paging_free_table(pte_addr(e));
    }
}

void paging_free_user_tables(uintptr_t root)
{
    paging_release_user_root(root);
    pte_t *top = P2V(root);
    for (int i = 0; i < PT_ROOT_USER_ENTRIES; i++) {
        pte_t e = top[i];
        if (!pte_is_table(e))
            continue;
        free_level(pte_table(e), PT_LEVELS - 1);
        paging_free_table(pte_addr(e));
        top[i] = 0;
    }
    paging_free_table(root);
}
