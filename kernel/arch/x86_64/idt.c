#define KLOG_SUBSYS "trap"
#include <arch/trap.h>
#include <arch/gdt.h>
#include <arch/cpu.h>
#include <arch/io.h>
#include <console.h>
#include <klog.h>
#include <debug/panic.h>
#include <mm/vmm.h>
#include <arch/irq.h>
#include <sched/sched.h>
#include <sched/thread.h>
#include <sched/proc.h>
#include <sched/user.h>
#include <ipc/signal.h>

struct idt_entry {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t ist;
    uint8_t type_attr;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t reserved;
} __packed;

struct idt_ptr {
    uint16_t limit;
    uint64_t base;
} __packed;

extern uintptr_t isr_table[256];

static struct idt_entry idt[256] __aligned(16);

static const char *const exception_names[32] = {
    "#DE divide error", "#DB debug", "NMI", "#BP breakpoint",
    "#OF overflow", "#BR bound range", "#UD invalid opcode", "#NM device not available",
    "#DF double fault", "coprocessor segment overrun", "#TS invalid TSS", "#NP segment not present",
    "#SS stack fault", "#GP general protection", "#PF page fault", "reserved",
    "#MF x87 error", "#AC alignment check", "#MC machine check", "#XM SIMD error",
    "#VE virtualization", "#CP control protection", "reserved", "reserved",
    "reserved", "reserved", "reserved", "reserved",
    "#HV hypervisor injection", "#VC VMM communication", "#SX security", "reserved",
};

static void idt_set_gate(int vec, uintptr_t handler, uint8_t ist, uint8_t dpl)
{
    idt[vec].offset_low = handler & 0xffff;
    idt[vec].selector = GDT_KERNEL_CODE;
    idt[vec].ist = ist;
    idt[vec].type_attr = 0x8e | (uint8_t)(dpl << 5);   /* present, interrupt gate */
    idt[vec].offset_mid = (handler >> 16) & 0xffff;
    idt[vec].offset_high = (uint32_t)(handler >> 32);
    idt[vec].reserved = 0;
}

static void pic_mask_all(void)
{
    /* Remap the legacy PICs away from the exception vectors and mask every
     * line. The local APIC replaces them in M5. */
    outb(0x20, 0x11); io_wait();
    outb(0xa0, 0x11); io_wait();
    outb(0x21, 0x20); io_wait();
    outb(0xa1, 0x28); io_wait();
    outb(0x21, 0x04); io_wait();
    outb(0xa1, 0x02); io_wait();
    outb(0x21, 0x01); io_wait();
    outb(0xa1, 0x01); io_wait();
    outb(0x21, 0xff);
    outb(0xa1, 0xff);
}

void idt_load(void)
{
    struct idt_ptr ptr = { sizeof idt - 1, (uint64_t)idt };
    __asm__ volatile("lidt %0" : : "m"(ptr));
}

void idt_init(void)
{
    for (int i = 0; i < 256; i++)
        idt_set_gate(i, isr_table[i], 0, 0);
    idt_set_gate(T_DBLFLT, isr_table[T_DBLFLT], 1, 0);
    idt_load();
    pic_mask_all();
}

/* Record what the CPU pushed for an entry from user mode so a fault on the
 * way back can be compared against it. */
static void trap_record_user_entry(const struct trapframe *tf)
{
    struct cpu *c = cpu_current();
    c->last_user_vector = tf->vector;
    c->last_user_error = tf->error;
    c->last_user_rip = tf->rip;
    c->last_user_cs = tf->cs;
    c->last_user_rsp = tf->rsp;
    c->last_user_ss = tf->ss;
    c->last_user_cr3 = read_cr3();
    c->last_user_frame = (void *)tf;
    if (c->current)
        c->current->kentry_frame = (struct trapframe *)tf;
}

void trap_dump_extra(const struct trapframe *tf)
{
    if ((tf->cs & 3) != 0)
        return;
    struct idt_ptr gdtr, idtr;
    uint16_t tr, cs, ss, ds;
    uint64_t cr0, cr4;
    __asm__ volatile("sgdt %0" : "=m"(gdtr));
    __asm__ volatile("sidt %0" : "=m"(idtr));
    __asm__ volatile("str %0" : "=r"(tr));
    __asm__ volatile("movw %%cs, %0" : "=r"(cs));
    __asm__ volatile("movw %%ss, %0" : "=r"(ss));
    __asm__ volatile("movw %%ds, %0" : "=r"(ds));
    __asm__ volatile("movq %%cr0, %0" : "=r"(cr0));
    __asm__ volatile("movq %%cr4, %0" : "=r"(cr4));
    /* The five word frame a faulting iretq was about to consume: rip,
     * cs, rflags, rsp, ss. It ends at the top of the kernel stack, so
     * nothing beyond it is read. */
    const uint64_t *frame = (const uint64_t *)tf->rsp;
    kprintf("frame at rsp: rip=%016lx cs=%04lx rflags=%016lx rsp=%016lx ss=%04lx\n",
            frame[0], frame[1], frame[2], frame[3], frame[4]);
    kprintf("live cs=%x ss=%x ds=%x tr=%x cr0=%lx cr4=%lx efer=%lx\n",
            cs, ss, ds, tr, cr0, cr4, rdmsr(MSR_EFER));
    kprintf("gdtr=%016lx/%x idtr=%016lx/%x gs_base=%016lx kernel_gs_base=%016lx\n",
            gdtr.base, gdtr.limit, idtr.base, idtr.limit,
            rdmsr(MSR_GS_BASE), rdmsr(MSR_KERNEL_GS_BASE));
    kprintf("star=%016lx lstar=%016lx sfmask=%016lx\n",
            rdmsr(MSR_STAR), rdmsr(MSR_LSTAR), rdmsr(MSR_SFMASK));
    const uint64_t *gdt = (const uint64_t *)gdtr.base;
    kprintf("gdt:");
    for (unsigned i = 0; i * 8 <= gdtr.limit && i < 16; i++)
        kprintf(" %016lx", gdt[i]);
    kprintf("\n");
    struct cpu *c = cpu_current();
    kprintf("cpu %u self=%p kstack_top=%p tss.rsp0=%016lx current=%p vm=%p\n",
            c->id, c->self, c->kstack_top, tss_get_rsp0(), c->current, c->vm);
    kprintf("last user entry: vector=%lu error=%lx rip=%016lx cs=%04lx rsp=%016lx ss=%04lx cr3=%lx frame=%p\n",
            c->last_user_vector, c->last_user_error, c->last_user_rip, c->last_user_cs,
            c->last_user_rsp, c->last_user_ss, c->last_user_cr3, c->last_user_frame);
}

void trap_dump_frame(const struct trapframe *tf)
{
    kprintf("vector=%lu error=0x%lx\n", tf->vector, tf->error);
    kprintf("RIP=%016lx CS=%04lx RFLAGS=%016lx\n", tf->rip, tf->cs, tf->rflags);
    kprintf("RSP=%016lx SS=%04lx\n", tf->rsp, tf->ss);
    kprintf("RAX=%016lx RBX=%016lx RCX=%016lx RDX=%016lx\n", tf->rax, tf->rbx, tf->rcx, tf->rdx);
    kprintf("RSI=%016lx RDI=%016lx RBP=%016lx\n", tf->rsi, tf->rdi, tf->rbp);
    kprintf("R8 =%016lx R9 =%016lx R10=%016lx R11=%016lx\n", tf->r8, tf->r9, tf->r10, tf->r11);
    kprintf("R12=%016lx R13=%016lx R14=%016lx R15=%016lx\n", tf->r12, tf->r13, tf->r14, tf->r15);
    kprintf("CR2=%016lx CR3=%016lx\n", read_cr2(), read_cr3());
}

void trap_dispatch(struct trapframe *tf)
{
    if ((tf->cs & 3) == 3)
        trap_record_user_entry(tf);
    if (tf->vector < 32) {
        uintptr_t cr2 = read_cr2();
        if (tf->vector == T_PGFLT && vmm_handle_fault(tf, cr2))
            return;
        kprintf("\nexception %lu: %s\n", tf->vector, exception_names[tf->vector]);
        if (tf->vector == T_PGFLT) {
            kprintf("page fault at %016lx: %s %s %s%s\n", cr2,
                    (tf->error & 4) ? "user" : "kernel",
                    (tf->error & 2) ? "write" : "read",
                    (tf->error & 1) ? "protection violation" : "not present",
                    (tf->error & 16) ? " (instruction fetch)" : "");
        }
        if ((tf->cs & 3) == 3) {
            struct thread *t = thread_current();
            if (signal_fault(t->proc, SIGSEGV)) {
                signal_deliver(tf);
                sched_preempt();
                return;
            }
            klog_error("process %s (pid %d) killed by exception %lu at rip %lx",
                       t->proc->name, t->proc->pid, tf->vector, tf->rip);
            trap_dump_frame(tf);
            user_fault_exit(PROC_STATUS_SIGNALED(SIGSEGV));
        }
        panic_trap(tf, "unhandled exception %lu (%s)", tf->vector, exception_names[tf->vector]);
    }
    irq_dispatch(tf);
    /* Returning to user mode is the only preemption point for user threads. */
    if ((tf->cs & 3) == 3) {
        proc_exit_check();
        signal_deliver(tf);
        sched_preempt();
    }
}
