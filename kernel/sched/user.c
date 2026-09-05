#define KLOG_SUBSYS "user"
#include <sched/user.h>
#include <arch/fpu.h>
#include <sched/proc.h>
#include <sched/thread.h>
#include <sched/sched.h>
#include <sched/elf.h>
#include <fs/vfs.h>
#include <fs/fdtable.h>
#include <arch/gdt.h>
#include <arch/syscall.h>
#include <arch/cpu.h>
#include <mm/vmm.h>
#include <mm/vma.h>
#include <mm/slab.h>
#include <lib/string.h>
#include <kassert.h>
#include <klog.h>
#include <errno.h>

/* First code of a user thread, in kernel mode on its own stack. The entry
 * frame prepared by the creator is copied to the stack and entered. */
static void user_thread_entry(void *arg)
{
    struct trapframe frame = *(struct trapframe *)arg;
    kfree(arg);
    thread_current()->user_frame = NULL;
    user_enter(&frame);
}

static void frame_init(struct trapframe *tf, uintptr_t rip, uintptr_t rsp)
{
    memset(tf, 0, sizeof *tf);
    tf->rip = rip;
    tf->cs = GDT_USER_CODE | 3;
    tf->rflags = RFLAGS_IF | 0x2;
    tf->rsp = rsp;
    tf->ss = GDT_USER_DATA | 3;
}

/* Build a new address space from an ELF on the initrd. */
/* The main stack size for a new image: RLIMIT_STACK of the creating
 * process, kept between 64 KiB and 1 GiB and page aligned. */
static size_t stack_size_for(struct proc *p)
{
    uint64_t lim = p ? proc_rlimit_cur(p, RLIMIT_STACK) : 8UL << 20;
    if (lim == RLIM_INFINITY || lim > (1UL << 30))
        lim = 1UL << 30;
    if (lim < USER_STACK_MIN)
        lim = USER_STACK_MIN;
    return ALIGN_UP(lim, PAGE_SIZE);
}

static int load_image(const char *path, char *const argv[], char *const envp[],
                      struct vmspace **vm_out, uintptr_t *entry, uintptr_t *rsp, size_t stack_size)
{
    struct file *f;
    int r = vfs_open(path, O_RDONLY, 0, &f);
    if (r < 0)
        return r;
    if (!S_ISREG(f->inode->mode)) {
        file_put(f);
        return -EACCES;
    }
    size_t size = f->inode->size;
    void *image = kmalloc(size ? size : 1);
    if (!image) {
        file_put(f);
        return -ENOMEM;
    }
    size_t got = 0;
    while (got < size) {
        long n = file_read(f, (char *)image + got, size - got);
        if (n <= 0) {
            r = n < 0 ? (int)n : -EIO;
            break;
        }
        got += (size_t)n;
    }
    file_put(f);
    if (r < 0) {
        kfree(image);
        return r;
    }
    struct vmspace *vm = vmspace_create();
    if (!vm) {
        kfree(image);
        return -ENOMEM;
    }
    r = elf_load(vm, image, size, entry);
    kfree(image);
    if (r == 0)
        r = user_stack_setup(vm, argv, envp, rsp, stack_size);
    if (r < 0) {
        vma_remove_all(vm);
        vmspace_destroy(vm);
        return r;
    }
    *vm_out = vm;
    return 0;
}

/* Give a new process the console on descriptors 0, 1 and 2. All three are
 * opened read/write, as a terminal is on Unix, so a program whose input is
 * a pipe can still read keys from the terminal behind its output (less,
 * for example) without opening /dev/console, which in a terminal window
 * would be another terminal. */
static int open_std_fds(struct proc *p)
{
    for (int fd = 0; fd < 3; fd++) {
        struct file *f;
        int r = vfs_open("/dev/console", O_RDWR, 0, &f);
        if (r < 0)
            return r;
        fdtable_install_at(&p->fds, f, fd);
    }
    return 0;
}

/* Allocate a user thread that enters tf. The caller finishes any per
 * thread state and then makes it runnable with sched_add; on another CPU
 * the thread starts running the moment it is queued. */
static struct thread *prepare_thread(struct proc *p, const char *name, struct trapframe *tf)
{
    struct thread *t = thread_alloc(p, name, user_thread_entry, tf, 0);
    if (!t)
        return NULL;
    t->user_frame = tf;
    if (thread_current()->proc != &kernel_proc) {
        t->sig_mask = thread_current()->sig_mask;
        t->fs_base = thread_current()->fs_base;
    }
    return t;
}

static struct thread *start_thread(struct proc *p, const char *name, struct trapframe *tf)
{
    struct thread *t = prepare_thread(p, name, tf);
    if (t)
        sched_add(t);
    return t;
}

struct proc *proc_create_user(const char *path, char *const argv[], char *const envp[],
                              struct proc *parent)
{
    struct vmspace *vm;
    uintptr_t entry, rsp;
    int r = load_image(path, argv, envp, &vm, &entry, &rsp, stack_size_for(parent));
    if (r < 0) {
        klog_error("cannot load %s: %d", path, r);
        return NULL;
    }
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    struct proc *p = proc_alloc(base, parent);
    if (!p)
        goto fail_vm;
    p->vm = vm;
    if (open_std_fds(p) < 0)
        goto fail_proc;
    struct trapframe *tf = kmalloc(sizeof *tf);
    if (!tf)
        goto fail_proc;
    frame_init(tf, entry, rsp);
    if (!start_thread(p, base, tf)) {
        kfree(tf);
        goto fail_proc;
    }
    klog_info("process %s (pid %d) from %s, entry %lx", p->name, p->pid, path, entry);
    return p;

fail_proc:
    p->vm = NULL;
    proc_free(p);
fail_vm:
    vma_remove_all(vm);
    vmspace_destroy(vm);
    return NULL;
}

struct proc *proc_fork(struct trapframe *tf)
{
    struct thread *cur = thread_current();
    struct proc *parent = cur->proc;
    struct proc *child = proc_alloc(parent->name, parent);
    if (!child)
        return NULL;
    child->vm = vmspace_fork(parent->vm);
    if (!child->vm) {
        proc_free(child);
        return NULL;
    }
    fdtable_copy(&child->fds, &parent->fds);
    signal_copy(child, parent);
    spin_lock(&proc_tree_lock);
    child->pgid = parent->pgid;
    spin_unlock(&proc_tree_lock);
    struct trapframe *ctf = kmalloc(sizeof *ctf);
    if (!ctf)
        goto fail;
    *ctf = *tf;
    ctf->rax = 0;
    struct thread *t = prepare_thread(child, cur->name, ctf);
    if (!t) {
        kfree(ctf);
        goto fail;
    }
    /* The child starts with the parent's registers. Its FPU image must be
     * complete before it can run, so it is queued only afterwards. */
    fpu_save(t->fpu);
    t->fs_base = cur->fs_base;
    sched_add(t);
    return child;
fail:
    proc_free(child);
    return NULL;
}

int proc_exec(struct trapframe *tf, const char *path, char *const argv[], char *const envp[])
{
    struct thread *cur = thread_current();
    struct proc *p = cur->proc;
    spin_lock(&p->lock);
    int nthreads = p->nthreads;
    spin_unlock(&p->lock);
    if (nthreads != 1)
        return -EBUSY;

    struct vmspace *vm;
    uintptr_t entry, rsp;
    int r = load_image(path, argv, envp, &vm, &entry, &rsp, stack_size_for(p));
    if (r < 0)
        return r;

    struct vmspace *old = p->vm;
    p->vm = vm;
    vmspace_activate(vm);
    vma_remove_all(old);
    vmspace_destroy(old);
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    strlcpy(p->name, base, sizeof p->name);
    strlcpy(cur->name, base, sizeof cur->name);
    signal_reset_for_exec(p);
    fdtable_close_exec(&p->fds);
    fpu_init_state(cur->fpu);
    fpu_restore(cur->fpu);
    cur->fs_base = 0;                   /* the new image sets up its own thread local storage */
    wrmsr(MSR_FS_BASE, 0);
    frame_init(tf, entry, rsp);
    return 0;
}

int user_thread_create(uintptr_t entry, uintptr_t arg, uintptr_t stack)
{
    struct thread *cur = thread_current();
    struct trapframe *tf = kmalloc(sizeof *tf);
    if (!tf)
        return -ENOMEM;
    frame_init(tf, entry, stack);
    tf->rdi = arg;
    struct thread *t = start_thread(cur->proc, cur->name, tf);
    if (!t) {
        kfree(tf);
        return -ENOMEM;
    }
    return t->tid;
}

__noreturn void user_fault_exit(int status)
{
    proc_begin_exit(thread_current()->proc, status);
    thread_exit(0);
}
