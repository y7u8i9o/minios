#define KLOG_SUBSYS "pmm"
#include <mm/pmm.h>
#include <mm/memlayout.h>
#include <boot.h>
#include <arch/cpu.h>
#include <arch/smp.h>
#include <sync/spinlock.h>
#include <lib/string.h>
#include <kassert.h>
#include <klog.h>
#include <console.h>
#include <debug/panic.h>

/* Buddy allocator. Every field below, plus lru, flags and order of every
 * struct page, is protected by pmm_lock. */
static DEFINE_SPINLOCK(pmm_lock);
static struct page *page_array;
uint64_t pmm_max_pfn;                                 /* pages covered by page_array */
static struct list_head free_lists[PMM_MAX_ORDER + 1];
uint64_t pmm_free_count[PMM_MAX_ORDER + 1];           /* blocks per free list */
struct pmm_stats pmm_stats;

static_assert_kernel(sizeof(struct page) == 24, "struct page size");

uint64_t page_to_pfn(const struct page *page)
{
    return (uint64_t)(page - page_array);
}

struct page *pfn_to_page(uint64_t pfn)
{
    kassert(pfn < pmm_max_pfn);
    return &page_array[pfn];
}

uintptr_t page_to_phys(const struct page *page)
{
    return page_to_pfn(page) << PAGE_SHIFT;
}

struct page *phys_to_page(uintptr_t pa)
{
    return pfn_to_page(pa >> PAGE_SHIFT);
}

static inline bool block_is_free(uint64_t pfn, unsigned order)
{
    return pfn < pmm_max_pfn && (page_array[pfn].flags & PG_FREE) &&
           page_array[pfn].order == order;
}

/* Insert a free block, merging with its buddy as far as possible. */
static void free_block(uint64_t pfn, unsigned order)
{
    kassert(spin_holding(&pmm_lock));
    kassert(IS_ALIGNED(pfn, 1UL << order));

    while (order < PMM_MAX_ORDER) {
        uint64_t buddy = pfn ^ (1UL << order);
        if (!block_is_free(buddy, order))
            break;
        list_del(&page_array[buddy].lru);
        page_array[buddy].flags &= ~PG_FREE;
        pmm_free_count[order]--;
        pfn = MIN(pfn, buddy);
        order++;
    }
    struct page *head = &page_array[pfn];
    head->order = (uint8_t)order;
    head->flags |= PG_FREE;
    list_add(&head->lru, &free_lists[order]);
    pmm_free_count[order]++;
}

/* Hand a physical range to the allocator in maximal aligned blocks. */
static void free_range(uint64_t start_pfn, uint64_t end_pfn)
{
    while (start_pfn < end_pfn) {
        unsigned order = PMM_MAX_ORDER;
        while (order > 0 &&
               (!IS_ALIGNED(start_pfn, 1UL << order) || start_pfn + (1UL << order) > end_pfn))
            order--;
        for (uint64_t i = 0; i < (1UL << order); i++)
            page_array[start_pfn + i].flags = 0;
        free_block(start_pfn, order);
        pmm_stats.total_pages += 1UL << order;
        pmm_stats.free_pages += 1UL << order;
        start_pfn += 1UL << order;
    }
}

static const char *memmap_type_name(uint64_t type)
{
    switch (type) {
    case LIMINE_MEMMAP_USABLE:                 return "usable";
    case LIMINE_MEMMAP_RESERVED:               return "reserved";
    case LIMINE_MEMMAP_ACPI_RECLAIMABLE:       return "acpi reclaimable";
    case LIMINE_MEMMAP_ACPI_NVS:               return "acpi nvs";
    case LIMINE_MEMMAP_BAD_MEMORY:             return "bad";
    case LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE: return "bootloader reclaimable";
    case LIMINE_MEMMAP_EXECUTABLE_AND_MODULES: return "kernel and modules";
    case LIMINE_MEMMAP_FRAMEBUFFER:            return "framebuffer";
    default:                                   return "unknown";
    }
}

void pmm_init(void)
{
    uint64_t max_addr = 0;
    uint64_t usable_bytes = 0;
    uint64_t reclaimable_bytes = 0;
    size_t usable_regions = 0;

    for (size_t i = 0; i < bootinfo.memmap_count; i++) {
        const struct limine_memmap_entry *e = &bootinfo.memmap[i];
        klog_debug("memmap %016lx-%016lx %s", e->base, e->base + e->length,
                   memmap_type_name(e->type));
        if (e->type == LIMINE_MEMMAP_USABLE) {
            usable_bytes += e->length;
            usable_regions++;
            if (e->base + e->length > max_addr)
                max_addr = e->base + e->length;
        } else if (e->type == LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE) {
            reclaimable_bytes += e->length;
        }
    }
    if (!max_addr)
        panic("no usable memory in the memory map");

    pmm_max_pfn = max_addr >> PAGE_SHIFT;
    uint64_t array_bytes = ALIGN_UP(pmm_max_pfn * sizeof(struct page), PAGE_SIZE);

    /* Carve the page array from the first usable region large enough. */
    uint64_t array_phys = 0;
    for (size_t i = 0; i < bootinfo.memmap_count; i++) {
        const struct limine_memmap_entry *e = &bootinfo.memmap[i];
        if (e->type == LIMINE_MEMMAP_USABLE && e->length >= array_bytes) {
            array_phys = e->base;
            break;
        }
    }
    if (!array_phys)
        panic("no region large enough for the page array (%lu bytes)", array_bytes);

    page_array = P2V(array_phys);
    memset(page_array, 0, array_bytes);
    for (uint64_t pfn = 0; pfn < pmm_max_pfn; pfn++)
        page_array[pfn].flags = PG_RESERVED;
    for (unsigned o = 0; o <= PMM_MAX_ORDER; o++) {
        list_init(&free_lists[o]);
        pmm_free_count[o] = 0;
    }

    spin_lock(&pmm_lock);
    for (size_t i = 0; i < bootinfo.memmap_count; i++) {
        const struct limine_memmap_entry *e = &bootinfo.memmap[i];
        if (e->type != LIMINE_MEMMAP_USABLE)
            continue;
        uint64_t start = ALIGN_UP(e->base, PAGE_SIZE) >> PAGE_SHIFT;
        uint64_t end = ALIGN_DOWN(e->base + e->length, PAGE_SIZE) >> PAGE_SHIFT;
        if (e->base == array_phys) {
            uint64_t array_end = (array_phys + array_bytes) >> PAGE_SHIFT;
            pmm_stats.reserved_pages += array_end - start;
            start = array_end;
        }
        if (start < end)
            free_range(start, end);
    }
    spin_unlock(&pmm_lock);

    klog_info("%lu MiB usable in %zu regions below %lx, %lu KiB reclaimable after boot",
              usable_bytes >> 20, usable_regions, max_addr, reclaimable_bytes >> 10);
    klog_info("%lu pages managed by a buddy allocator with %u orders, page array %lu KiB at %lx",
              pmm_stats.total_pages, PMM_MAX_ORDER + 1, array_bytes >> 10, array_phys);
}

void pmm_reclaim_bootloader(void)
{
    uint64_t reclaimed = 0;
    spin_lock(&pmm_lock);
    for (size_t i = 0; i < bootinfo.memmap_count; i++) {
        const struct limine_memmap_entry *e = &bootinfo.memmap[i];
        if (e->type != LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE)
            continue;
        uint64_t start = ALIGN_UP(e->base, PAGE_SIZE) >> PAGE_SHIFT;
        uint64_t end = MIN(ALIGN_DOWN(e->base + e->length, PAGE_SIZE) >> PAGE_SHIFT, pmm_max_pfn);
        if (start < end) {
            free_range(start, end);
            reclaimed += end - start;
        }
    }
    uint64_t free = pmm_stats.free_pages, total = pmm_stats.total_pages;
    spin_unlock(&pmm_lock);
    klog_info("reclaimed %lu bootloader pages, %lu of %lu pages free", reclaimed, free, total);
}

struct page *pmm_alloc(unsigned order)
{
    kassert(order <= PMM_MAX_ORDER);
    spin_lock(&pmm_lock);

    unsigned o = order;
    while (o <= PMM_MAX_ORDER && list_empty(&free_lists[o]))
        o++;
    if (o > PMM_MAX_ORDER) {
        spin_unlock(&pmm_lock);
        return NULL;
    }

    struct page *page = list_first_entry(&free_lists[o], struct page, lru);
    list_del(&page->lru);
    page->flags &= ~PG_FREE;
    page->refcount = 0;
    pmm_free_count[o]--;

    /* Split until the block has the requested order, returning the upper
     * halves to their free lists. */
    uint64_t pfn = page_to_pfn(page);
    while (o > order) {
        o--;
        struct page *buddy = &page_array[pfn + (1UL << o)];
        buddy->order = (uint8_t)o;
        buddy->flags |= PG_FREE;
        list_add(&buddy->lru, &free_lists[o]);
        pmm_free_count[o]++;
    }
    page->order = (uint8_t)order;
    pmm_stats.free_pages -= 1UL << order;

    spin_unlock(&pmm_lock);
    return page;
}

void pmm_free(struct page *page, unsigned order)
{
    kassert(page != NULL);
    kassert(order <= PMM_MAX_ORDER);
    uint64_t pfn = page_to_pfn(page);
    kassert(pfn < pmm_max_pfn);
    kassert(IS_ALIGNED(pfn, 1UL << order));

    spin_lock(&pmm_lock);
    if (page->flags & PG_FREE)
        panic("pmm_free: double free of pfn %lx", pfn);
    if (page->flags & PG_RESERVED)
        panic("pmm_free: freeing reserved pfn %lx", pfn);
    if (page->order != order)
        panic("pmm_free: pfn %lx allocated with order %u, freed with %u", pfn, page->order, order);
    free_block(pfn, order);
    pmm_stats.free_pages += 1UL << order;
    spin_unlock(&pmm_lock);
}

struct page *pmm_alloc_page(void)
{
    struct cpu *c = cpu_current();
    spin_lock(&c->pmm_cache_lock);
    if (c->pmm_cache_count) {
        struct page *page = c->pmm_cache[--c->pmm_cache_count];
        page->flags &= (uint16_t)~PG_CPUCACHE;
        spin_unlock(&c->pmm_cache_lock);
        return page;
    }
    spin_unlock(&c->pmm_cache_lock);

    struct page *batch[8];
    unsigned n = 0;
    while (n < ARRAY_SIZE(batch) && (batch[n] = pmm_alloc(0)) != NULL)
        n++;
    if (n == 0)
        return NULL;
    spin_lock(&c->pmm_cache_lock);
    for (unsigned i = 1; i < n; i++) {
        if (c->pmm_cache_count < ARRAY_SIZE(c->pmm_cache)) {
            batch[i]->flags |= PG_CPUCACHE;
            c->pmm_cache[c->pmm_cache_count++] = batch[i];
        } else {
            spin_unlock(&c->pmm_cache_lock);
            pmm_free(batch[i], 0);
            spin_lock(&c->pmm_cache_lock);
        }
    }
    spin_unlock(&c->pmm_cache_lock);
    return batch[0];
}

void pmm_split_block(struct page *head, unsigned order)
{
    uint64_t pfn = page_to_pfn(head);
    kassert(IS_ALIGNED(pfn, 1UL << order));
    spin_lock(&pmm_lock);
    kassert(!(head->flags & PG_FREE) && head->order == order);
    for (uint64_t i = 0; i < (1UL << order); i++) {
        page_array[pfn + i].order = 0;
        page_array[pfn + i].flags &= (uint16_t)~PG_FREE;
        if (i)
            page_array[pfn + i].refcount = 0;   /* the count of the block is on the head */
    }
    spin_unlock(&pmm_lock);
}

bool page_put(struct page *page)
{
    uint32_t old = __atomic_fetch_sub(&page->refcount, 1, __ATOMIC_SEQ_CST);
    kassert(old > 0);
    if (old == 1) {
        pmm_free_page(page);
        return true;
    }
    return false;
}

void pmm_free_page(struct page *page)
{
    struct cpu *c = cpu_current();
    struct page *drain[16];
    unsigned n = 0;
    spin_lock(&c->pmm_cache_lock);
    if (page->flags & PG_CPUCACHE)
        panic("pmm_free_page: double free of pfn %lx", page_to_pfn(page));
    if (c->pmm_cache_count == ARRAY_SIZE(c->pmm_cache)) {
        while (n < ARRAY_SIZE(drain)) {
            drain[n] = c->pmm_cache[--c->pmm_cache_count];
            drain[n]->flags &= (uint16_t)~PG_CPUCACHE;
            n++;
        }
    }
    page->flags |= PG_CPUCACHE;
    c->pmm_cache[c->pmm_cache_count++] = page;
    spin_unlock(&c->pmm_cache_lock);
    for (unsigned i = 0; i < n; i++)
        pmm_free(drain[i], 0);
}

void pmm_reclaim_cpu_caches(void)
{
    for (unsigned cpu = 0; cpu < smp_cpu_count(); cpu++) {
        struct cpu *c = cpu_by_id(cpu);
        struct page *drain[ARRAY_SIZE(c->pmm_cache)];
        unsigned n = 0;
        spin_lock(&c->pmm_cache_lock);
        while (c->pmm_cache_count) {
            struct page *page = c->pmm_cache[--c->pmm_cache_count];
            page->flags &= (uint16_t)~PG_CPUCACHE;
            drain[n++] = page;
        }
        spin_unlock(&c->pmm_cache_lock);
        for (unsigned i = 0; i < n; i++)
            pmm_free(drain[i], 0);
    }
}

void pmm_get_stats(struct pmm_stats *out)
{
    spin_lock(&pmm_lock);
    *out = pmm_stats;
    spin_unlock(&pmm_lock);
    for (unsigned i = 0; i < smp_cpu_count(); i++) {
        struct cpu *c = cpu_by_id(i);
        spin_lock(&c->pmm_cache_lock);
        out->free_pages += c->pmm_cache_count;
        spin_unlock(&c->pmm_cache_lock);
    }
}

void pmm_get_free_counts(uint64_t *out)
{
    spin_lock(&pmm_lock);
    for (unsigned o = 0; o <= PMM_MAX_ORDER; o++)
        out[o] = pmm_free_count[o];
    spin_unlock(&pmm_lock);
    for (unsigned i = 0; i < smp_cpu_count(); i++) {
        struct cpu *c = cpu_by_id(i);
        spin_lock(&c->pmm_cache_lock);
        out[0] += c->pmm_cache_count;
        spin_unlock(&c->pmm_cache_lock);
    }
}

void pmm_dump_stats(void)
{
    struct pmm_stats st;
    uint64_t counts[PMM_MAX_ORDER + 1];
    pmm_get_stats(&st);
    pmm_get_free_counts(counts);
    kprintf("pmm: %lu pages total, %lu free, %lu reserved (page array)\n",
            st.total_pages, st.free_pages, st.reserved_pages);
    for (unsigned o = 0; o <= PMM_MAX_ORDER; o++)
        kprintf("  order %2u (%7lu KiB): %lu free blocks\n", o, (PAGE_SIZE << o) >> 10, counts[o]);
}

bool pmm_is_ram(uintptr_t pa)
{
    /* A reserved frame (firmware memory, a framebuffer in RAM such as the
     * ramfb of virt) has no reference count and is treated as device
     * memory. PG_RESERVED changes only before the first user mapping
     * (pmm_reclaim_bootloader), so it is read without pmm_lock. */
    uint64_t pfn = pa >> PAGE_SHIFT;
    return pfn < pmm_max_pfn && !(page_array[pfn].flags & PG_RESERVED);
}
