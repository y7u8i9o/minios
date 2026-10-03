#include <tests/ktest.h>
#include <mm/pmm.h>
#include <mm/memlayout.h>
#include <lib/string.h>
#include <console.h>

/* M3: allocation and free patterns leave the free lists exactly as they were,
 * which proves that split blocks coalesce again. */
#define NPAGES 1024

static struct page *pages[NPAGES];

static void snapshot(uint64_t *counts, struct pmm_stats *st)
{
    pmm_reclaim_cpu_caches();
    pmm_get_free_counts(counts);
    pmm_get_stats(st);
}

static void expect_same(const uint64_t *before, const struct pmm_stats *sb, const char *what)
{
    uint64_t after[PMM_MAX_ORDER + 1];
    struct pmm_stats sa;
    snapshot(after, &sa);
    ktest_assert(sa.free_pages == sb->free_pages, "%s: free pages %lu, expected %lu",
                 what, sa.free_pages, sb->free_pages);
    for (unsigned o = 0; o <= PMM_MAX_ORDER; o++) {
        ktest_assert(after[o] == before[o], "%s: order %u has %lu blocks, expected %lu",
                     what, o, after[o], before[o]);
    }
}

static void test_pmm(void)
{
    uint64_t before[PMM_MAX_ORDER + 1];
    struct pmm_stats sb;
    snapshot(before, &sb);
    pmm_dump_stats();
    ktest_assert(sb.free_pages > NPAGES * 4, "too little free memory: %lu pages", sb.free_pages);

    /* Single pages, freed in reverse order. */
    for (int i = 0; i < NPAGES; i++) {
        pages[i] = pmm_alloc_page();
        ktest_assert(pages[i] != NULL, "alloc %d failed", i);
        ktest_assert(!(pages[i]->flags & PG_FREE), "allocated page marked free");
        uint64_t *p = P2V(page_to_phys(pages[i]));
        p[0] = (uint64_t)i;
        p[511] = ~(uint64_t)i;
    }
    for (int i = 0; i < NPAGES; i++) {
        for (int j = i + 1; j < NPAGES; j++)
            ktest_assert(pages[i] != pages[j], "page %d returned twice", i);
    }
    for (int i = 0; i < NPAGES; i++) {
        uint64_t *p = P2V(page_to_phys(pages[i]));
        ktest_assert(p[0] == (uint64_t)i && p[511] == ~(uint64_t)i, "page %d contents corrupted", i);
    }
    struct pmm_stats mid;
    pmm_get_stats(&mid);
    ktest_assert(mid.free_pages == sb.free_pages - NPAGES, "free count after alloc: %lu", mid.free_pages);
    for (int i = NPAGES - 1; i >= 0; i--)
        pmm_free_page(pages[i]);
    expect_same(before, &sb, "reverse free");

    /* Same pages freed in interleaved order: even indices, then odd. */
    for (int i = 0; i < NPAGES; i++)
        pages[i] = pmm_alloc_page();
    for (int i = 0; i < NPAGES; i += 2)
        pmm_free_page(pages[i]);
    for (int i = 1; i < NPAGES; i += 2)
        pmm_free_page(pages[i]);
    expect_same(before, &sb, "interleaved free");

    /* Every order, checking alignment of the returned block. */
    for (unsigned o = 0; o <= PMM_MAX_ORDER; o++) {
        struct page *pg = pmm_alloc(o);
        ktest_assert(pg != NULL, "order %u alloc failed", o);
        ktest_assert(IS_ALIGNED(page_to_phys(pg), PAGE_SIZE << o), "order %u block misaligned", o);
        ktest_assert(pg->order == o, "order %u block records order %u", o, pg->order);
        pmm_free(pg, o);
        expect_same(before, &sb, "single order alloc/free");
    }

    /* Split one large block into pieces, free them in scrambled order. */
    struct page *big = pmm_alloc(PMM_MAX_ORDER);
    ktest_assert(big != NULL, "order %u alloc failed", PMM_MAX_ORDER);
    pmm_free(big, PMM_MAX_ORDER);
    struct page *parts[16];
    for (int i = 0; i < 16; i++) {
        parts[i] = pmm_alloc(PMM_MAX_ORDER - 4);
        ktest_assert(parts[i] != NULL, "part %d alloc failed", i);
    }
    static const int order_of_free[16] = { 5, 12, 0, 9, 14, 3, 7, 1, 11, 6, 15, 2, 8, 13, 4, 10 };
    for (int i = 0; i < 16; i++)
        pmm_free(parts[order_of_free[i]], PMM_MAX_ORDER - 4);
    expect_same(before, &sb, "scrambled free");

    /* Exhaustion at the top order returns NULL without damage. */
    struct page **blocks = (struct page **)P2V(page_to_phys(pmm_alloc(2)));
    size_t maxhold = (PAGE_SIZE << 2) / sizeof(struct page *);
    size_t nhold = 0;
    while (nhold < maxhold) {
        struct page *pg = pmm_alloc(PMM_MAX_ORDER);
        if (!pg)
            break;
        blocks[nhold++] = pg;
    }
    ktest_assert(nhold < maxhold, "did not exhaust order %u", PMM_MAX_ORDER);
    ktest_assert(pmm_alloc(PMM_MAX_ORDER) == NULL, "alloc succeeded after exhaustion");
    uint64_t counts[PMM_MAX_ORDER + 1];
    pmm_get_free_counts(counts);
    ktest_assert(counts[PMM_MAX_ORDER] == 0, "order %u list not empty", PMM_MAX_ORDER);
    for (size_t i = 0; i < nhold; i++)
        pmm_free(blocks[i], PMM_MAX_ORDER);
    pmm_free(phys_to_page(V2P(blocks)), 2);
    expect_same(before, &sb, "exhaustion");

    pmm_dump_stats();
}
KTEST_DEFINE_STAGE("pmm", test_pmm, KTEST_MEMORY);
