#define KLOG_SUBSYS "ksyms"
#include <debug/symbols.h>
#include <console.h>
#include <klog.h>

/* Blob layout produced by tools/gensyms. */
#define KSYMS_MAGIC 0x53594d4bu

struct ksyms_header {
    uint32_t magic;
    uint32_t count;
    uint32_t strtab_off;
    uint32_t strtab_len;
};

struct ksyms_entry {
    uint64_t addr;
    uint32_t size;
    uint32_t name_off;
};

extern char __ksyms_start[], __ksyms_end[];

static const struct ksyms_header *hdr;
static const struct ksyms_entry *entries;
static const char *strtab;
static size_t count;

void ksyms_init(void)
{
    size_t len = (size_t)(__ksyms_end - __ksyms_start);
    if (len < sizeof(struct ksyms_header)) {
        klog_warn("symbol table absent");
        return;
    }
    const struct ksyms_header *h = (const struct ksyms_header *)__ksyms_start;
    if (h->magic != KSYMS_MAGIC) {
        klog_warn("symbol table has bad magic %x", h->magic);
        return;
    }
    hdr = h;
    entries = (const struct ksyms_entry *)(__ksyms_start + sizeof *h);
    strtab = __ksyms_start + h->strtab_off;
    count = h->count;
    klog_info("%zu symbols, %zu bytes", count, len);
}

size_t ksyms_count(void)
{
    return count;
}

const char *ksyms_lookup(uintptr_t addr, uintptr_t *offset, size_t *size)
{
    if (!count)
        return NULL;
    /* Binary search for the last entry with entry.addr <= addr. */
    size_t lo = 0, hi = count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (entries[mid].addr <= addr)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo == 0)
        return NULL;
    const struct ksyms_entry *e = &entries[lo - 1];
    /* Return addresses after a noreturn call land in the padding between
     * functions, so an address is attributed to the preceding symbol up to
     * the start of the next one. */
    size_t sz = lo < count ? entries[lo].addr - e->addr : e->size;
    if (sz && addr >= e->addr + sz)
        return NULL;
    if (offset)
        *offset = addr - e->addr;
    if (size)
        *size = sz;
    return strtab + e->name_off;
}

void ksyms_print_addr(uintptr_t addr)
{
    uintptr_t off;
    size_t size;
    const char *name = ksyms_lookup(addr, &off, &size);
    if (name)
        kprintf("[<%016lx>] %s+0x%lx/0x%zx", addr, name, off, size);
    else
        kprintf("[<%016lx>] ?", addr);
}
