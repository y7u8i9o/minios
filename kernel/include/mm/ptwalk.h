#pragma once
#include <kernel.h>
#include <arch/paging.h>

/* Range walks over the level 1 tables of an address space (A2). The walk
 * uses only the geometry and entry functions of <arch/paging.h>, so it is
 * correct for every architecture whose tables have 4 KiB pages at level 1. */

/* Return the level 1 table that maps *va, or NULL once *va reaches end.
 * Absent upper tables and 2 MiB blocks are skipped by advancing *va past
 * the range their entry covers. With parent, *parent receives the level 2
 * entry that points to the returned table. The caller must have acquired
 * the lock of the address space, and continues at the next address it
 * wants to examine. */
pte_t *pt_next_leaf_table(uintptr_t root, uintptr_t *va, uintptr_t end, pte_t **parent);
