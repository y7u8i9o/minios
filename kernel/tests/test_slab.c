#include <tests/ktest.h>
#include <mm/slab.h>
#include <mm/pmm.h>
#include <lib/string.h>
#include <console.h>

/* M5: allocation stress with pattern verification and exact page accounting. */
#define NOBJ 2000

static void *objs[NOBJ];
static size_t sizes[NOBJ];

static uint32_t rng_state = 12345;
static uint32_t rng(void)
{
    rng_state = rng_state * 1103515245u + 12345u;
    return rng_state >> 8;
}

static void fill(void *p, size_t n, unsigned seed)
{
    uint8_t *b = p;
    for (size_t i = 0; i < n; i++)
        b[i] = (uint8_t)(seed * 31 + i);
}

static void verify(const void *p, size_t n, unsigned seed, const char *what)
{
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++)
        ktest_assert(b[i] == (uint8_t)(seed * 31 + i), "%s: byte %zu of %p corrupted", what, i, p);
}

static void expect_free(uint64_t baseline, const char *what)
{
    struct pmm_stats st;
    slab_reclaim();
    pmm_get_stats(&st);
    ktest_assert(st.free_pages == baseline, "%s: %lu free pages, expected %lu", what, st.free_pages, baseline);
}

static void test_slab(void)
{
    struct pmm_stats st;
    slab_reclaim();
    pmm_get_stats(&st);
    uint64_t baseline = st.free_pages;

    /* Mixed sizes across every class and the large path. */
    for (int i = 0; i < NOBJ; i++) {
        uint32_t r = rng() % 100;
        sizes[i] = r < 60 ? 1 + rng() % 256 : r < 95 ? 1 + rng() % 8192 : 8193 + rng() % 40000;
        objs[i] = kmalloc(sizes[i]);
        ktest_assert(objs[i] != NULL, "kmalloc(%zu) failed at %d", sizes[i], i);
        ktest_assert(((uintptr_t)objs[i] & 15) == 0, "kmalloc(%zu) misaligned: %p", sizes[i], objs[i]);
        fill(objs[i], sizes[i], (unsigned)i);
    }
    for (int i = 0; i < NOBJ; i++)
        verify(objs[i], sizes[i], (unsigned)i, "first pass");
    /* Free every other object, reallocate with new sizes, verify everything. */
    for (int i = 0; i < NOBJ; i += 2) {
        kfree(objs[i]);
        objs[i] = NULL;
    }
    for (int i = 0; i < NOBJ; i += 2) {
        sizes[i] = 1 + rng() % 4096;
        objs[i] = kmalloc(sizes[i]);
        ktest_assert(objs[i] != NULL, "realloc %d failed", i);
        fill(objs[i], sizes[i], (unsigned)i + 7);
    }
    for (int i = 0; i < NOBJ; i++)
        verify(objs[i], sizes[i], (unsigned)(i % 2 ? i : i + 7), "second pass");
    slab_dump_stats();
    for (int i = NOBJ - 1; i >= 0; i--)
        kfree(objs[i]);
    expect_free(baseline, "after kmalloc stress");

    /* A dedicated cache with an odd object size. */
    struct kmem_cache *c = kmem_cache_create("test-100", 100, 32);
    ktest_assert(c != NULL, "kmem_cache_create failed");
    for (int i = 0; i < NOBJ; i++) {
        objs[i] = kmem_cache_alloc(c);
        ktest_assert(objs[i] != NULL, "cache alloc %d failed", i);
        ktest_assert(((uintptr_t)objs[i] & 31) == 0, "cache object misaligned");
        fill(objs[i], 100, (unsigned)i);
    }
    for (int i = 0; i < NOBJ; i++) {
        verify(objs[i], 100, (unsigned)i, "cache");
        for (int j = i + 1; j < i + 8 && j < NOBJ; j++)
            ktest_assert(objs[i] != objs[j], "duplicate object %d", i);
    }
    for (int i = 0; i < NOBJ; i += 3)
        kmem_cache_free(c, objs[i]);
    for (int i = 1; i < NOBJ; i += 3)
        kmem_cache_free(c, objs[i]);
    for (int i = 2; i < NOBJ; i += 3)
        kmem_cache_free(c, objs[i]);
    kmem_cache_destroy(c);
    expect_free(baseline, "after cache test");

    /* kzalloc and kfree(NULL). */
    uint64_t *z = kzalloc(3000);
    ktest_assert(z != NULL, "kzalloc failed");
    for (int i = 0; i < 3000 / 8; i++)
        ktest_assert(z[i] == 0, "kzalloc not zeroed");
    kfree(z);
    kfree(NULL);
    expect_free(baseline, "after kzalloc");
}
KTEST_DEFINE_STAGE("slab", test_slab, KTEST_MEMORY);

/* Overrunning an object must be caught by the redzone on free. */
static void test_slab_redzone(void)
{
    char *p = kmalloc(40);
    ktest_assert(p != NULL, "kmalloc failed");
    memset(p, 'x', 70);
    kfree(p);
    ktest_fail("redzone violation not detected");
}
KTEST_DEFINE_STAGE("slab_redzone", test_slab_redzone, KTEST_MEMORY);
