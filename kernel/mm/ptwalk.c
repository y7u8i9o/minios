#include <mm/ptwalk.h>

pte_t *pt_next_leaf_table(uintptr_t root, uintptr_t *vap, uintptr_t end, pte_t **parent)
{
    uintptr_t va = *vap;
    while (va < end) {
        pte_t *table = P2V(root);
        int level = PT_LEVELS;
        for (;;) {
            pte_t *e = &table[PT_INDEX(va, level)];
            if (!pte_is_table(*e))
                break;
            if (level == 2) {
                if (parent)
                    *parent = e;
                *vap = va;
                return pte_table(*e);
            }
            table = pte_table(*e);
            level--;
        }
        /* The entry at level is absent or a block: skip its range. */
        uintptr_t next = ALIGN_DOWN(va, PT_LEVEL_SIZE(level)) + PT_LEVEL_SIZE(level);
        if (next <= va)
            break;
        va = next;
    }
    *vap = va < end ? end : va;
    return NULL;
}
