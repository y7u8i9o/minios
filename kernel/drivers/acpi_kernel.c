/* The kernel side of uACPI (third_party/uacpi, include/uacpi/kernel_api.h):
 * memory mapping, I/O ports, PCI configuration space, allocation, time,
 * mutexes, events, spinlocks, interrupts and deferred work, for the full
 * mode with the AML interpreter (docs/design/acpi.md). The functions run
 * on both architectures. On aarch64 the early table reading of
 * devtree_init uses the first four of them before vmm_init. */
#define KLOG_SUBSYS "acpi"
#include <drivers/acpi.h>
#include <arch/platform.h>
#include <arch/cpu.h>
#include <arch/irq.h>
#include <boot.h>
#include <klog.h>
#include <errno.h>
#include <lib/string.h>
#include <mm/memlayout.h>
#include <mm/slab.h>
#include <mm/vmm.h>
#include <drivers/timer.h>
#include <sched/thread.h>
#include <sched/wait.h>
#include <sync/mutex.h>
#include <sync/spinlock.h>
#include <uacpi/kernel_api.h>

/* ---- tables and memory ---- */

uacpi_status uacpi_kernel_get_rsdp(uacpi_phys_addr *out_rsdp_address)
{
    if (!bootinfo.rsdp_phys)
        return UACPI_STATUS_NOT_FOUND;
    *out_rsdp_address = bootinfo.rsdp_phys;
    return UACPI_STATUS_OK;
}

static bool late;               /* written once by acpi_kernel_late */

/* The memory map types in the direct map. Limine maps the reserved mapped
 * regions as well under base revision 4, which the early table reading of
 * aarch64 uses. The direct map of the kernel, which vmm_init builds and
 * acpi_kernel_late follows, does not contain them. The RSDP of a PC lies
 * in such a region. */
static bool in_direct_map(uint64_t pa, uint64_t len)
{
    for (size_t i = 0; i < bootinfo.memmap_count; i++) {
        const struct limine_memmap_entry *e = &bootinfo.memmap[i];
        switch (e->type) {
        case LIMINE_MEMMAP_USABLE:
        case LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE:
        case LIMINE_MEMMAP_EXECUTABLE_AND_MODULES:
        case LIMINE_MEMMAP_ACPI_RECLAIMABLE:
        case LIMINE_MEMMAP_ACPI_NVS:
            break;
        case LIMINE_MEMMAP_RESERVED_MAPPED:
            if (late)
                continue;
            break;
        default:
            continue;
        }
        if (pa >= e->base && pa + len <= e->base + e->length)
            return true;
    }
    return false;
}

/* Device memory outside the direct map, such as an operation region of
 * the AML code, is mapped once per page range and the mapping is reused.
 * vmm_map_mmio does not return its address space, so the list prevents a
 * loss of address space when uACPI maps the same range again. maps_lock
 * protects maps and nmaps. */
#define MAX_MAPS 64
struct acpi_map {
    uint64_t pa;
    size_t size;
    uintptr_t va;
};
static struct acpi_map maps[MAX_MAPS];
static unsigned nmaps;
static DEFINE_SPINLOCK(maps_lock);

void acpi_kernel_late(void)
{
    late = true;
}

void *uacpi_kernel_map(uacpi_phys_addr addr, uacpi_size len)
{
    if (in_direct_map(addr, len))
        return P2V(addr);
    if (!late) {
        klog_warn("table at %lx, %lu bytes, is outside the direct map", (uintptr_t)addr, (unsigned long)len);
        return UACPI_MAP_FAILED;
    }
    uint64_t base = ALIGN_DOWN(addr, PAGE_SIZE);
    size_t size = ALIGN_UP(addr + len, PAGE_SIZE) - base;
    spin_lock(&maps_lock);
    for (unsigned i = 0; i < nmaps; i++)
        if (base >= maps[i].pa && base + size <= maps[i].pa + maps[i].size) {
            uintptr_t va = maps[i].va + (base - maps[i].pa);
            spin_unlock(&maps_lock);
            return (void *)(va + (addr - base));
        }
    spin_unlock(&maps_lock);
    void *va = vmm_map_mmio(base, size, VM_KERNEL_RW | VM_NOCACHE);
    if (!va)
        return UACPI_MAP_FAILED;
    spin_lock(&maps_lock);
    if (nmaps < MAX_MAPS)
        maps[nmaps++] = (struct acpi_map){ base, size, (uintptr_t)va };
    spin_unlock(&maps_lock);
    return (uint8_t *)va + (addr - base);
}

void uacpi_kernel_unmap(void *addr, uacpi_size len)
{
}

void uacpi_kernel_log(uacpi_log_level level, const uacpi_char *msg)
{
    char line[160];
    strlcpy(line, msg, sizeof line);
    size_t n = strlen(line);
    if (n && line[n - 1] == '\n')
        line[n - 1] = '\0';
    switch (level) {
    case UACPI_LOG_ERROR: klog_error("uacpi: %s", line); break;
    case UACPI_LOG_WARN:  klog_warn("uacpi: %s", line); break;
    case UACPI_LOG_INFO:  klog_info("uacpi: %s", line); break;
    default:              klog_debug("uacpi: %s", line); break;
    }
}

void *uacpi_kernel_alloc(uacpi_size size)
{
    return kmalloc(size);
}

void uacpi_kernel_free(void *mem)
{
    kfree(mem);
}

/* ---- PCI configuration space ---- */

/* A handle is the address of the function. Only segment 0 exists on the
 * QEMU machines. */
struct pci_handle {
    uint8_t bus, slot, func;
};

uacpi_status uacpi_kernel_pci_device_open(uacpi_pci_address address, uacpi_handle *out_handle)
{
    if (address.segment != 0)
        return UACPI_STATUS_UNIMPLEMENTED;
    struct pci_handle *h = kmalloc(sizeof *h);
    if (!h)
        return UACPI_STATUS_OUT_OF_MEMORY;
    *h = (struct pci_handle){ address.bus, address.device, address.function };
    *out_handle = h;
    return UACPI_STATUS_OK;
}

void uacpi_kernel_pci_device_close(uacpi_handle handle)
{
    kfree(handle);
}

static uint32_t pci_word(uacpi_handle handle, uacpi_size offset)
{
    const struct pci_handle *h = handle;
    return platform_pci_read32(h->bus, h->slot, h->func, (uint8_t)(offset & ~3u));
}

/* A write of 8 or 16 bits reads the word, replaces its part and writes
 * it back, because the platform accesses whole words. */
static void pci_merge(uacpi_handle handle, uacpi_size offset, unsigned width, uint32_t value)
{
    const struct pci_handle *h = handle;
    unsigned shift = (offset & 3) * 8;
    uint32_t mask = (width == 4 ? 0xffffffffu : ((1u << (width * 8)) - 1)) << shift;
    uint32_t word = width == 4 ? 0 : pci_word(handle, offset) & ~mask;
    platform_pci_write32(h->bus, h->slot, h->func, (uint8_t)(offset & ~3u), word | ((value << shift) & mask));
}

uacpi_status uacpi_kernel_pci_read8(uacpi_handle device, uacpi_size offset, uacpi_u8 *value)
{
    *value = (uint8_t)(pci_word(device, offset) >> ((offset & 3) * 8));
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_pci_read16(uacpi_handle device, uacpi_size offset, uacpi_u16 *value)
{
    *value = (uint16_t)(pci_word(device, offset) >> ((offset & 2) * 8));
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_pci_read32(uacpi_handle device, uacpi_size offset, uacpi_u32 *value)
{
    *value = pci_word(device, offset);
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_pci_write8(uacpi_handle device, uacpi_size offset, uacpi_u8 value)
{
    pci_merge(device, offset, 1, value);
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_pci_write16(uacpi_handle device, uacpi_size offset, uacpi_u16 value)
{
    pci_merge(device, offset, 2, value);
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_pci_write32(uacpi_handle device, uacpi_size offset, uacpi_u32 value)
{
    pci_merge(device, offset, 4, value);
    return UACPI_STATUS_OK;
}

/* ---- I/O ports ---- */

/* A handle is the first port of the range. */
uacpi_status uacpi_kernel_io_map(uacpi_io_addr base, uacpi_size len, uacpi_handle *out_handle)
{
    if (!platform_has_ports() || base + len > 0x10000)
        return UACPI_STATUS_UNIMPLEMENTED;
    *out_handle = (uacpi_handle)(uintptr_t)base;
    return UACPI_STATUS_OK;
}

void uacpi_kernel_io_unmap(uacpi_handle handle)
{
}

static uint16_t port_of(uacpi_handle handle, uacpi_size offset)
{
    return (uint16_t)((uintptr_t)handle + offset);
}

uacpi_status uacpi_kernel_io_read8(uacpi_handle h, uacpi_size offset, uacpi_u8 *out_value)
{
    *out_value = (uint8_t)platform_port_read(port_of(h, offset), 1);
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_io_read16(uacpi_handle h, uacpi_size offset, uacpi_u16 *out_value)
{
    *out_value = (uint16_t)platform_port_read(port_of(h, offset), 2);
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_io_read32(uacpi_handle h, uacpi_size offset, uacpi_u32 *out_value)
{
    *out_value = platform_port_read(port_of(h, offset), 4);
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_io_write8(uacpi_handle h, uacpi_size offset, uacpi_u8 in_value)
{
    platform_port_write(port_of(h, offset), 1, in_value);
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_io_write16(uacpi_handle h, uacpi_size offset, uacpi_u16 in_value)
{
    platform_port_write(port_of(h, offset), 2, in_value);
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_io_write32(uacpi_handle h, uacpi_size offset, uacpi_u32 in_value)
{
    platform_port_write(port_of(h, offset), 4, in_value);
    return UACPI_STATUS_OK;
}

/* ---- time ---- */

uacpi_u64 uacpi_kernel_get_nanoseconds_since_boot(void)
{
    return timer_ns();
}

void uacpi_kernel_stall(uacpi_u8 usec)
{
    uint64_t end = timer_ns() + (uint64_t)usec * 1000;
    while (timer_ns() < end)
        cpu_relax();
}

void uacpi_kernel_sleep(uacpi_u64 msec)
{
    sleep_ms(msec);
}

/* ---- mutexes, events and spinlocks ---- */

uacpi_handle uacpi_kernel_create_mutex(void)
{
    struct mutex *m = kmalloc(sizeof *m);
    if (m)
        mutex_init(m, "acpi mutex");
    return m;
}

void uacpi_kernel_free_mutex(uacpi_handle handle)
{
    kfree(handle);
}

/* A timeout of 0xffff waits without limit, 0 tries once. The kernel mutex
 * has no timed lock, so a limited wait retries every millisecond. */
uacpi_status uacpi_kernel_acquire_mutex(uacpi_handle handle, uacpi_u16 timeout)
{
    struct mutex *m = handle;
    if (timeout == 0xffff) {
        mutex_lock(m);
        return UACPI_STATUS_OK;
    }
    uint64_t deadline = timer_ms() + timeout;
    for (;;) {
        if (mutex_trylock(m))
            return UACPI_STATUS_OK;
        if (timer_ms() >= deadline)
            return UACPI_STATUS_TIMEOUT;
        sleep_ms(1);
    }
}

void uacpi_kernel_release_mutex(uacpi_handle handle)
{
    mutex_unlock(handle);
}

/* An event is a counter with a wait queue. lock protects count. */
struct acpi_event {
    struct spinlock lock;
    unsigned count;
    struct waitq wq;
};

uacpi_handle uacpi_kernel_create_event(void)
{
    struct acpi_event *e = kmalloc(sizeof *e);
    if (!e)
        return NULL;
    spinlock_init(&e->lock, "acpi event");
    e->count = 0;
    waitq_init(&e->wq, "acpi event");
    return e;
}

void uacpi_kernel_free_event(uacpi_handle handle)
{
    kfree(handle);
}

uacpi_bool uacpi_kernel_wait_for_event(uacpi_handle handle, uacpi_u16 timeout)
{
    struct acpi_event *e = handle;
    uint64_t deadline = timer_ms() + timeout;
    spin_lock(&e->lock);
    while (e->count == 0) {
        if (timeout == 0 || (timeout != 0xffff && timer_ms() >= deadline)) {
            spin_unlock(&e->lock);
            return UACPI_FALSE;
        }
        if (timeout == 0xffff)
            waitq_wait(&e->wq, &e->lock);
        else
            waitq_wait_timeout(&e->wq, &e->lock, deadline);
    }
    e->count--;
    spin_unlock(&e->lock);
    return UACPI_TRUE;
}

void uacpi_kernel_signal_event(uacpi_handle handle)
{
    struct acpi_event *e = handle;
    spin_lock(&e->lock);
    e->count++;
    waitq_wake_one(&e->wq);
    spin_unlock(&e->lock);
}

void uacpi_kernel_reset_event(uacpi_handle handle)
{
    struct acpi_event *e = handle;
    spin_lock(&e->lock);
    e->count = 0;
    spin_unlock(&e->lock);
}

uacpi_thread_id uacpi_kernel_get_thread_id(void)
{
    return thread_current();
}

uacpi_interrupt_state uacpi_kernel_disable_interrupts(void)
{
    return arch_irq_save();
}

void uacpi_kernel_restore_interrupts(uacpi_interrupt_state state)
{
    arch_irq_restore(state);
}

uacpi_handle uacpi_kernel_create_spinlock(void)
{
    struct spinlock *lk = kmalloc(sizeof *lk);
    if (lk)
        spinlock_init(lk, "acpi spinlock");
    return lk;
}

void uacpi_kernel_free_spinlock(uacpi_handle handle)
{
    kfree(handle);
}

/* spin_lock disables interrupts until the matching unlock, so the flags
 * of uACPI are unused. */
uacpi_cpu_flags uacpi_kernel_lock_spinlock(uacpi_handle handle)
{
    spin_lock(handle);
    return 0;
}

void uacpi_kernel_unlock_spinlock(uacpi_handle handle, uacpi_cpu_flags flags)
{
    spin_unlock(handle);
}

uacpi_status uacpi_kernel_handle_firmware_request(uacpi_firmware_request *req)
{
    if (req->type == UACPI_FIRMWARE_REQUEST_TYPE_FATAL)
        klog_error("the firmware reports a fatal error");
    return UACPI_STATUS_OK;
}

/* ---- interrupts ---- */

/* The SCI and the interrupts of the Generic Event Device. A handle is the
 * entry of the table. The table is written before the interrupt is
 * enabled and read by the interrupt handler without a lock. */
#define MAX_INTERRUPTS 8
static struct acpi_interrupt {
    uacpi_interrupt_handler handler;
    uacpi_handle ctx;
    bool used;
} interrupts[MAX_INTERRUPTS];
static DEFINE_SPINLOCK(interrupts_lock);  /* protects the used flags */

static void interrupt_entry(struct trapframe *tf, void *arg)
{
    struct acpi_interrupt *i = arg;
    i->handler(i->ctx);
}


uacpi_status uacpi_kernel_install_interrupt_handler(uacpi_u32 irq, uacpi_interrupt_handler handler, uacpi_handle ctx,
                                                    uacpi_handle *out_irq_handle)
{
    spin_lock(&interrupts_lock);
    struct acpi_interrupt *slot = NULL;
    for (unsigned i = 0; i < MAX_INTERRUPTS && !slot; i++)
        if (!interrupts[i].used)
            slot = &interrupts[i];
    if (slot)
        *slot = (struct acpi_interrupt){ handler, ctx, true };
    spin_unlock(&interrupts_lock);
    if (!slot)
        return UACPI_STATUS_OUT_OF_MEMORY;
    int r = irq_route_gsi(irq, acpi_irq_flags(irq), interrupt_entry, slot);
    if (r < 0) {
        klog_warn("cannot route interrupt %u: %d", irq, r);
        slot->used = false;
        return UACPI_STATUS_INTERNAL_ERROR;
    }
    *out_irq_handle = slot;
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_uninstall_interrupt_handler(uacpi_interrupt_handler handler, uacpi_handle irq_handle)
{
    /* The kernel removes no interrupt route. The handler stays installed
     * and ignores further interrupts. */
    return UACPI_STATUS_UNIMPLEMENTED;
}

/* ---- deferred work ---- */

/* A ring of work items for the thread "acpi". Interrupt handlers fill it,
 * so it allocates nothing. work_lock protects work, work_head, work_tail,
 * running and worker. */
#define WORK_SLOTS 32
static struct {
    void (*fn)(void *arg);
    void *arg;
} work[WORK_SLOTS];
static unsigned work_head, work_tail, running;
static DEFINE_SPINLOCK(work_lock);
static DEFINE_WAITQ(work_wq);
static DEFINE_WAITQ(work_done_wq);
static struct thread *worker;

static void work_thread(void *arg)
{
    spin_lock(&work_lock);
    for (;;) {
        if (work_head == work_tail) {
            running = 0;
            waitq_wake_all(&work_done_wq);
            waitq_wait(&work_wq, &work_lock);
            continue;
        }
        void (*fn)(void *) = work[work_tail % WORK_SLOTS].fn;
        void *fn_arg = work[work_tail % WORK_SLOTS].arg;
        work_tail++;
        running = 1;
        spin_unlock(&work_lock);
        fn(fn_arg);
        spin_lock(&work_lock);
    }
}

bool acpi_defer(void (*fn)(void *arg), void *arg)
{
    spin_lock(&work_lock);
    bool ok = worker && work_head - work_tail < WORK_SLOTS;
    if (ok) {
        work[work_head % WORK_SLOTS].fn = fn;
        work[work_head % WORK_SLOTS].arg = arg;
        work_head++;
        waitq_wake_one(&work_wq);
    }
    spin_unlock(&work_lock);
    if (!ok)
        klog_warn("cannot defer ACPI work");
    return ok;
}

/* Starts the thread of the deferred work. acpi_init calls it before the
 * events are enabled. */
void acpi_start_worker(void)
{
    struct thread *t = thread_create("acpi", work_thread, NULL, 0);
    spin_lock(&work_lock);
    worker = t;
    spin_unlock(&work_lock);
}

uacpi_status uacpi_kernel_schedule_work(uacpi_work_type type, uacpi_work_handler handler, uacpi_handle ctx)
{
    return acpi_defer(handler, ctx) ? UACPI_STATUS_OK : UACPI_STATUS_OUT_OF_MEMORY;
}

uacpi_status uacpi_kernel_wait_for_work_completion(void)
{
    spin_lock(&work_lock);
    while (work_head != work_tail || running)
        waitq_wait(&work_done_wq, &work_lock);
    spin_unlock(&work_lock);
    return UACPI_STATUS_OK;
}
