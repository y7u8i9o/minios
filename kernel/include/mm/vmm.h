#pragma once
#include <kernel.h>
#include <sync/spinlock.h>
#include <sync/percpu.h>
#include <lib/list.h>
#include <arch/cpu.h>
#include <arch/memlayout.h>

/* Mapping flags. */
#define VM_READ     (1u << 0)
#define VM_WRITE    (1u << 1)
#define VM_EXEC     (1u << 2)
#define VM_USER     (1u << 3)
#define VM_NOCACHE  (1u << 4)   /* uncacheable, device memory */
#define VM_WC       (1u << 5)   /* write combining, framebuffers */
#define VM_GLOBAL   (1u << 6)   /* kept across CR3 loads, kernel mappings */
#define VM_MMAP     (1u << 7)   /* region created by mmap, removable by munmap */
#define VM_DEVICE   (1u << 8)   /* frames are device memory, not managed pages */
#define VM_SHARED   (1u << 9)   /* shared mapping: fork shares frames without copy on write */
#define VM_FILE     (1u << 10)  /* backed by a file through struct mapping (M37) */
#define VM_HUGE     (1u << 11)  /* anonymous region backed by 2 MiB frames where possible (M39) */
#define VM_DONTFORK (1u << 12)  /* MADV_DONTFORK: not copied by vmspace_fork (M38) */
#define VM_SEQUENTIAL (1u << 13) /* MADV_SEQUENTIAL recorded (M38) */
#define VM_RANDOM   (1u << 14)  /* MADV_RANDOM recorded (M38) */
#define VM_PROT_MASK (VM_READ | VM_WRITE | VM_EXEC)
#define VM_KERNEL_RW (VM_READ | VM_WRITE | VM_GLOBAL)

/* Virtual layout. The addresses of the regions are defined by the
 * architecture (<arch/memlayout.h>); the sizes below are generic. */
#define USER_STACK_SIZE (1UL << 20)     /* legacy default, exec sizes the stack by RLIMIT_STACK (M40) */
#define USER_STACK_MIN  (64UL << 10)
#define KSTACK_SIZE     (16UL << 10)
#define KSTACK_SLOT     (KSTACK_SIZE + PAGE_SIZE)
#define KSTACK_SLOTS    4096

/* An address space. lock protects the page tables reachable from pt_root,
 * the region list and the heap break. The kernel's instance is
 * kernel_vmspace and its lock is kvm_lock. */
struct vmspace {
    uintptr_t pt_root;          /* physical address of the root page table */
    struct spinlock lock;
    struct list_head vmas;      /* struct vma, sorted by start */
    uintptr_t brk_start;        /* heap region start, 0 if none */
    uintptr_t brk;              /* current break, page aligned */
    bool pinned;                /* kswapd must not evict from this space */
    struct list_head link;      /* vmspaces, protected by vmspaces_lock */
    cpu_mask_t cpu_mask;        /* CPUs with this space in CR3, atomic updates in vmspace_activate */
    struct percpu_counter resident; /* resident user pages, lock-free sum */
    uint64_t tlb_tag;           /* architecture TLB tag: ASID and generation on aarch64, asid_lock */
};

extern struct vmspace kernel_vmspace;
/* Every user address space, for the swap daemon. */
extern struct list_head vmspaces;
extern struct spinlock vmspaces_lock;

void vmm_init(void);

int vmm_map(struct vmspace *vm, uintptr_t va, uintptr_t pa, size_t size, unsigned flags);
int vmm_unmap(struct vmspace *vm, uintptr_t va, size_t size);
int vmm_protect(struct vmspace *vm, uintptr_t va, size_t size, unsigned flags);
bool vmm_translate(struct vmspace *vm, uintptr_t va, uintptr_t *pa, unsigned *flags);

struct vmspace *vmspace_create(void);
/* Unmap every 4 KiB frame mapped in the lower half, dropping a reference
 * on each. */
void vmspace_free_user_pages(struct vmspace *vm);
void vmspace_destroy(struct vmspace *vm);
void vmspace_activate(struct vmspace *vm);
struct vmspace *vmspace_current(void);

#include <mm/tlb.h>

/* Map device memory into the kernel space. Returns the virtual address. */
void *vmm_map_mmio(uintptr_t pa, size_t size, unsigned flags);

/* Kernel stacks: KSTACK_SIZE bytes preceded by an unmapped guard page.
 * Returns the top of the stack. */
void *kstack_alloc(void);
void kstack_free(void *top);

/* Page fault entry from the trap handler. Returns true if resolved. */
struct trapframe;
bool vmm_handle_fault(struct trapframe *tf, uintptr_t addr);
