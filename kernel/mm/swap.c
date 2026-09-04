#define KLOG_SUBSYS "swap"
#include <mm/swap.h>
#include <mm/vmm.h>
#include <mm/vma.h>
#include <mm/pmm.h>
#include <mm/slab.h>
#include <mm/filemap.h>
#include <mm/huge.h>
#include <mm/memlayout.h>
#include <arch/paging.h>
#include <block/blockdev.h>
#include <fs/vfs.h>
#include <fs/devfs.h>
#include <sched/thread.h>
#include <sched/wait.h>
#include <sync/mutex.h>
#include <drivers/timer.h>
#include <lib/string.h>
#include <lib/printf.h>
#include <kassert.h>
#include <klog.h>
#include <errno.h>
#include <debug/panic.h>

#define SECTORS_PER_SLOT (PAGE_SIZE / 512)
/* kswapd reclaims when free pages drop below LOW until they reach HIGH. */
/* User frames are not handed out below RESERVE so kernel allocations
 * made on the swap path (request bounce buffers) keep succeeding. */
#define WATERMARK_RESERVE 256
#define WATERMARK_HIGH    768
#define EVICT_BATCH       32      /* pages written per swap request */
#define SWAPIN_CLUSTER    16      /* consecutive slots read per fault */
#define SCAN_BUDGET    2048     /* page table entries examined per lock hold */

static struct blockdev *swapdev;
/* Slot bitmap and counters. Protected by swap_lock. */
static DEFINE_SPINLOCK(swap_lock);
static uint64_t *slot_bitmap;
static uint64_t nslots, free_slots, slot_hint;
static uint64_t swapped_out, swapped_in, lazy_freed;
/* Serializes swap I/O so a swap in never reads a slot whose write is in
 * flight. */
static struct mutex swap_io_lock;
/* Threads waiting for kswapd to free memory. Condition lock is swap_lock. */
static DEFINE_WAITQ(swap_waitq);

bool swap_enabled(void)
{
    return swapdev != NULL;
}

uint64_t swap_alloc_slot(void)
{
    spin_lock(&swap_lock);
    for (uint64_t n = 0; n < nslots; n++) {
        uint64_t s = (slot_hint + n) % nslots;
        if (s == 0)
            continue;   /* slot 0 is never used so a zero entry means "none" */
        if (!(slot_bitmap[s / 64] & (1UL << (s % 64)))) {
            slot_bitmap[s / 64] |= 1UL << (s % 64);
            free_slots--;
            slot_hint = s + 1;
            spin_unlock(&swap_lock);
            return s;
        }
    }
    spin_unlock(&swap_lock);
    return 0;
}

/* Allocate up to n consecutive slots. Returns the count (at least 1 on
 * success, 0 when swap is full) and the first slot through *first. */
static unsigned swap_alloc_run(unsigned n, uint64_t *first)
{
    spin_lock(&swap_lock);
    for (uint64_t tries = 0; tries < nslots; tries++) {
        uint64_t s = (slot_hint + tries) % nslots;
        if (s == 0 || (slot_bitmap[s / 64] & (1UL << (s % 64))))
            continue;
        unsigned run = 1;
        while (run < n && s + run < nslots &&
               !(slot_bitmap[(s + run) / 64] & (1UL << ((s + run) % 64))))
            run++;
        for (unsigned i = 0; i < run; i++)
            slot_bitmap[(s + i) / 64] |= 1UL << ((s + i) % 64);
        free_slots -= run;
        slot_hint = s + run;
        *first = s;
        spin_unlock(&swap_lock);
        return run;
    }
    spin_unlock(&swap_lock);
    return 0;
}

void swap_free_slot(uint64_t slot)
{
    kassert(slot > 0 && slot < nslots);
    spin_lock(&swap_lock);
    kassert(slot_bitmap[slot / 64] & (1UL << (slot % 64)));
    slot_bitmap[slot / 64] &= ~(1UL << (slot % 64));
    free_slots++;
    spin_unlock(&swap_lock);
}

void swap_get_stats(struct swap_stats *out)
{
    spin_lock(&swap_lock);
    out->total_slots = nslots ? nslots - 1 : 0;
    out->free_slots = free_slots;
    out->swapped_out = swapped_out;
    out->swapped_in = swapped_in;
    out->lazy_freed = lazy_freed;
    spin_unlock(&swap_lock);
}

/* ---- eviction ---- */

/* Clock hand: the space and address where the last scan stopped. Only
 * kswapd touches these. */
static struct vmspace *hand_vm;
static uintptr_t hand_va;

/* Scan vm from *cursor for an evictable page: a present user page whose
 * frame is not shared and whose accessed bit is clear. Accessed bits seen
 * on the way are cleared. Returns the entry or NULL, leaving *cursor at
 * the next address to examine. Caller holds vm->lock. */
static uint64_t *find_victim(struct vmspace *vm, uintptr_t *cursor, uintptr_t *victim_va)
{
    unsigned budget = SCAN_BUDGET;
    uint64_t *pml4 = P2V(vm->pml4_phys);
    uintptr_t va = *cursor;
    while (va <= USER_TOP && budget) {
        uint64_t e4 = pml4[PML4_INDEX(va)];
        if (!(e4 & PTE_P)) {
            va = ALIGN_DOWN(va, 1UL << 39) + (1UL << 39);
            continue;
        }
        uint64_t e3 = ((uint64_t *)P2V(e4 & PTE_ADDR_MASK))[PDPT_INDEX(va)];
        if (!(e3 & PTE_P)) {
            va = ALIGN_DOWN(va, 1UL << 30) + (1UL << 30);
            continue;
        }
        uint64_t e2 = ((uint64_t *)P2V(e3 & PTE_ADDR_MASK))[PD_INDEX(va)];
        if (!(e2 & PTE_P) || (e2 & PTE_PS)) {
            va = ALIGN_DOWN(va, 1UL << 21) + (1UL << 21);   /* empty or a huge page */
            continue;
        }
        uint64_t *pt = P2V(e2 & PTE_ADDR_MASK);
        for (unsigned i = PT_INDEX(va); i < PT_ENTRIES && budget; i++, va += PAGE_SIZE, budget--) {
            uint64_t e = pt[i];
            if (!(e & PTE_P) || !(e & PTE_U) || !pmm_is_ram(e & PTE_ADDR_MASK))
                continue;
            struct page *pg = phys_to_page(e & PTE_ADDR_MASK);
            if (e & PTE_LAZYFREE) {
                /* MADV_FREE: a page written since keeps its data, a clean
                 * one is discarded instead of swapped. */
                if (e & PTE_D) {
                    pt[i] = e & ~PTE_LAZYFREE;
                } else {
                    pt[i] = 0;
                    tlb_flush_range(vm, va, PAGE_SIZE);
                    page_put(pg);
                    spin_lock(&swap_lock);
                    lazy_freed++;
                    spin_unlock(&swap_lock);
                    continue;
                }
            }
            if (__atomic_load_n(&pg->refcount, __ATOMIC_SEQ_CST) != 1)
                continue;
            if (e & PTE_A) {
                pt[i] = e & ~PTE_A;
                tlb_flush_range(vm, va, PAGE_SIZE);
                continue;
            }
            *victim_va = va;
            *cursor = va + PAGE_SIZE;
            return &pt[i];
        }
    }
    *cursor = va;
    return NULL;
}

/* Evict a batch of pages into consecutive slots with one write. Returns
 * the number of pages written out. swap_io_lock is held from the moment
 * an entry is marked swapped until its data is on the disk, so a fault
 * on another CPU that reads the slot waits for the write. */
static unsigned evict_batch_locked(void)
{
    struct page *frames[EVICT_BATCH];
    unsigned nframes = 0;
    uint64_t first = 0;

    spin_lock(&vmspaces_lock);
    if (list_empty(&vmspaces)) {
        spin_unlock(&vmspaces_lock);
        return 0;
    }
    /* Resume at the hand if its space still exists. */
    struct vmspace *vm = NULL;
    struct list_head *pos;
    size_t count = 0;
    list_for_each(pos, &vmspaces) {
        count++;
        if (list_entry(pos, struct vmspace, link) == hand_vm)
            vm = hand_vm;
    }
    if (!vm) {
        vm = list_first_entry(&vmspaces, struct vmspace, link);
        hand_va = 0;
    }
    unsigned want = swap_alloc_run(EVICT_BATCH, &first);
    if (!want) {
        spin_unlock(&vmspaces_lock);
        return 0;
    }
    /* Walk the spaces round robin. Each visit examines a bounded number
     * of entries; accessed bits cleared on one visit make the page a
     * candidate on the next. */
    for (size_t step = 0; step < 128 * count && nframes < want; step++) {
        spin_lock(&vm->lock);
        if (!vm->pinned) {
            uintptr_t va;
            uint64_t *entry;
            while (nframes < want && (entry = find_victim(vm, &hand_va, &va)) != NULL) {
                frames[nframes] = phys_to_page(*entry & PTE_ADDR_MASK);
                *entry = ((first + nframes) << 12) | PTE_SWAPPED;
                tlb_flush_range(vm, va, PAGE_SIZE);
                nframes++;
            }
        } else {
            hand_va = USER_TOP + 1;
        }
        spin_unlock(&vm->lock);
        if (nframes < want && hand_va > USER_TOP) {
            vm = vm->link.next == &vmspaces ? list_first_entry(&vmspaces, struct vmspace, link)
                                            : list_entry(vm->link.next, struct vmspace, link);
            hand_va = 0;
        }
    }
    hand_vm = vm;
    spin_unlock(&vmspaces_lock);
    for (unsigned i = nframes; i < want; i++)
        swap_free_slot(first + i);
    if (!nframes)
        return 0;

    /* Gather the frames into one contiguous buffer and write it. Without
     * a large block available, fall back to one page at a time. */
    uint8_t *bounce = nframes > 1 ? kmalloc((size_t)nframes * PAGE_SIZE) : NULL;
    int r = 0;
    if (bounce) {
        for (unsigned i = 0; i < nframes; i++)
            memcpy(bounce + i * PAGE_SIZE, P2V(page_to_phys(frames[i])), PAGE_SIZE);
        r = blockdev_write(swapdev, first * SECTORS_PER_SLOT, nframes * SECTORS_PER_SLOT, bounce);
        kfree(bounce);
    } else {
        for (unsigned i = 0; i < nframes && r == 0; i++)
            r = blockdev_write(swapdev, (first + i) * SECTORS_PER_SLOT, SECTORS_PER_SLOT,
                               P2V(page_to_phys(frames[i])));
    }
    if (r < 0)
        panic("swap write failed: %d", r);
    for (unsigned i = 0; i < nframes; i++)
        page_put(frames[i]);
    spin_lock(&swap_lock);
    swapped_out += nframes;
    spin_unlock(&swap_lock);
    return nframes;
}

static unsigned evict_batch(void)
{
    mutex_lock(&swap_io_lock);
    unsigned n = evict_batch_locked();
    mutex_unlock(&swap_io_lock);
    return n;
}

/* True while kswapd holds frames or buffers of a batch in flight. */
static volatile bool evicting;

void swap_drain(void)
{
    while (evicting)
        sleep_ms(1);
}

static void kswapd(void *arg)
{
    for (;;) {
        struct pmm_stats st;
        pmm_get_stats(&st);
        if (st.free_pages >= WATERMARK_HIGH) {
            sleep_ms(10);
            continue;
        }
        evicting = true;
        unsigned evicted = evict_batch();
        evicting = false;
        spin_lock(&swap_lock);
        waitq_wake_all(&swap_waitq);
        spin_unlock(&swap_lock);
        if (!evicted)
            sleep_ms(10);
    }
}

struct page *swap_alloc_user_frame(void)
{
    for (int tries = 0; tries < 10000; tries++) {
        struct pmm_stats st;
        pmm_get_stats(&st);
        if (!swapdev || st.free_pages > WATERMARK_RESERVE) {
            struct page *pg = pmm_alloc_page();
            if (pg)
                return pg;
            if (!swapdev)
                return NULL;
        }
        /* Wait for kswapd to make progress. */
        spin_lock(&swap_lock);
        waitq_wait(&swap_waitq, &swap_lock);
        spin_unlock(&swap_lock);
    }
    return NULL;
}

/* ---- swap in ---- */

int swap_in_page(struct vmspace *vm, uintptr_t va)
{
    va = ALIGN_DOWN(va, PAGE_SIZE);
    spin_lock(&vm->lock);
    uint64_t *entry;
    int w = paging_walk(vm->pml4_phys, va, false, &entry);
    if (w != 1 || !(*entry & PTE_SWAPPED)) {
        spin_unlock(&vm->lock);
        return 0;   /* already resident */
    }
    uint64_t slot = *entry >> 12;
    /* Cluster: following pages whose slots follow this one were evicted
     * together and are likely to be used together. */
    unsigned n = 1;
    while (n < SWAPIN_CLUSTER && va + n * PAGE_SIZE <= USER_TOP) {
        uint64_t *e;
        if (paging_walk(vm->pml4_phys, va + n * PAGE_SIZE, false, &e) != 1 ||
            !(*e & PTE_SWAPPED) || (*e >> 12) != slot + n)
            break;
        n++;
    }
    spin_unlock(&vm->lock);

    struct page *pages[SWAPIN_CLUSTER];
    unsigned got = 0;
    while (got < n) {
        pages[got] = swap_alloc_user_frame();
        if (!pages[got])
            break;
        got++;
    }
    if (got == 0)
        return -ENOMEM;
    n = got;
    mutex_lock(&swap_io_lock);
    int r;
    uint8_t *bounce = n > 1 ? kmalloc((size_t)n * PAGE_SIZE) : NULL;
    if (bounce) {
        r = blockdev_read(swapdev, slot * SECTORS_PER_SLOT, n * SECTORS_PER_SLOT, bounce);
        for (unsigned i = 0; i < n && r == 0; i++)
            memcpy(P2V(page_to_phys(pages[i])), bounce + i * PAGE_SIZE, PAGE_SIZE);
        kfree(bounce);
    } else {
        n = 1;
        r = blockdev_read(swapdev, slot * SECTORS_PER_SLOT, SECTORS_PER_SLOT, P2V(page_to_phys(pages[0])));
    }
    mutex_unlock(&swap_io_lock);
    if (r < 0) {
        for (unsigned i = 0; i < got; i++)
            pmm_free_page(pages[i]);
        return r;
    }

    spin_lock(&vm->lock);
    for (unsigned i = 0; i < got; i++) {
        uintptr_t a = va + i * PAGE_SIZE;
        struct vma *v = i < n ? vma_find_locked(vm, a) : NULL;
        w = i < n ? paging_walk(vm->pml4_phys, a, false, &entry) : 0;
        if (w == 1 && (*entry & PTE_SWAPPED) && (*entry >> 12) == slot + i && v) {
            page_get(pages[i]);
            *entry = page_to_phys(pages[i]) | vma_pte_flags(v->flags);
            tlb_flush_range(vm, a, PAGE_SIZE);
            swap_free_slot(slot + i);
            spin_lock(&swap_lock);
            swapped_in++;
            spin_unlock(&swap_lock);
        } else {
            /* Brought in by another thread meanwhile, or not needed. */
            pmm_free_page(pages[i]);
        }
    }
    spin_unlock(&vm->lock);
    return 0;
}

/* First swapped entry at or after *va, or false. Caller holds vm->lock. */
static bool find_swapped(struct vmspace *vm, uintptr_t *va)
{
    uint64_t *pml4 = P2V(vm->pml4_phys);
    uintptr_t a = *va;
    while (a <= USER_TOP) {
        uint64_t e4 = pml4[PML4_INDEX(a)];
        if (!(e4 & PTE_P)) { a = ALIGN_DOWN(a, 1UL << 39) + (1UL << 39); continue; }
        uint64_t e3 = ((uint64_t *)P2V(e4 & PTE_ADDR_MASK))[PDPT_INDEX(a)];
        if (!(e3 & PTE_P)) { a = ALIGN_DOWN(a, 1UL << 30) + (1UL << 30); continue; }
        uint64_t e2 = ((uint64_t *)P2V(e3 & PTE_ADDR_MASK))[PD_INDEX(a)];
        if (!(e2 & PTE_P) || (e2 & PTE_PS)) { a = ALIGN_DOWN(a, 1UL << 21) + (1UL << 21); continue; }
        uint64_t *pt = P2V(e2 & PTE_ADDR_MASK);
        for (unsigned i = PT_INDEX(a); i < PT_ENTRIES; i++, a += PAGE_SIZE) {
            if (pt[i] & PTE_SWAPPED) {
                *va = a;
                return true;
            }
        }
    }
    return false;
}

int swap_in_all(struct vmspace *vm)
{
    uintptr_t va = 0;
    for (;;) {
        spin_lock(&vm->lock);
        bool found = find_swapped(vm, &va);
        spin_unlock(&vm->lock);
        if (!found)
            return 0;
        int r = swap_in_page(vm, va);
        if (r < 0)
            return r;
        va += PAGE_SIZE;
    }
}

/* ---- /dev/meminfo ---- */

static long meminfo_read(struct file *f, char *buf, size_t n, uint64_t *pos)
{
    struct pmm_stats st;
    struct swap_stats ss;
    struct filemap_stats fs;
    struct huge_stats hs;
    pmm_get_stats(&st);
    swap_get_stats(&ss);
    filemap_get_stats(&fs);
    huge_get_stats(&hs);
    char text[512];
    int len = ksnprintf(text, sizeof text,
                        "MemTotal: %lu kB\nMemFree: %lu kB\nSwapTotal: %lu kB\nSwapFree: %lu kB\n"
                        "SwappedOut: %lu\nSwappedIn: %lu\nLazyFreed: %lu\nFileMapped: %lu\n"
                        "HugePages: %lu\nHugeSplits: %lu\nHugeFallbacks: %lu\n",
                        st.total_pages * 4, st.free_pages * 4, ss.total_slots * 4, ss.free_slots * 4,
                        ss.swapped_out, ss.swapped_in, ss.lazy_freed, fs.cached_pages,
                        hs.mapped, hs.splits, hs.fallbacks);
    if (*pos >= (uint64_t)len)
        return 0;
    size_t avail = (size_t)len - *pos;
    if (n > avail)
        n = avail;
    memcpy(buf, text + *pos, n);
    *pos += n;
    return (long)n;
}

static const struct file_ops meminfo_fops = { .read = meminfo_read };

void swap_init(void)
{
    devfs_register("meminfo", S_IFCHR | 0444, &meminfo_fops, NULL, 0);
    mutex_init(&swap_io_lock, "swap_io");
    struct blockdev *dev = blockdev_find("vdb");
    if (!dev) {
        klog_info("no swap device (vdb)");
        return;
    }
    nslots = blockdev_size(dev) / PAGE_SIZE;
    slot_bitmap = kzalloc(ALIGN_UP(nslots, 64) / 8);
    if (!slot_bitmap) {
        klog_error("cannot allocate the slot bitmap");
        return;
    }
    slot_bitmap[0] |= 1;     /* slot 0 reserved */
    free_slots = nslots - 1;
    slot_hint = 1;
    swapdev = dev;
    klog_info("%s: %lu slots (%lu MiB)", dev->name, free_slots, free_slots * PAGE_SIZE >> 20);
}

void swap_start_daemon(void)
{
    if (!swapdev)
        return;
    if (!thread_create("kswapd", kswapd, NULL, 0))
        klog_error("cannot start kswapd");
}
