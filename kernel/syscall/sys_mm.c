#include <syscall/syscalls.h>
#include <arch/trap.h>
#include <sched/thread.h>
#include <sched/proc.h>
#include <mm/vma.h>
#include <fs/vfs.h>
#include <fs/fdtable.h>
#include <minios/abi.h>
#include <errno.h>

long sys_sbrk(struct trapframe *tf)
{
    return vma_brk(thread_current()->proc->vm, (intptr_t)SYSARG0(tf));
}

/* mmap(addr, len, prot, flags, fd, off): anonymous private mappings only. */
long sys_mmap(struct trapframe *tf)
{
    uintptr_t addr = SYSARG0(tf);
    size_t len = SYSARG1(tf);
    int prot = (int)SYSARG2(tf);
    int flags = (int)SYSARG3(tf);
    int fd = (int)SYSARG4(tf);
    uint64_t off = SYSARG5(tf);
    if (flags & MAP_FIXED)
        return -EINVAL;
    unsigned vmflags = VM_READ;
    if (prot & PROT_WRITE)
        vmflags |= VM_WRITE;
    if (prot & PROT_EXEC)
        vmflags |= VM_EXEC;
    struct vmspace *vm = thread_current()->proc->vm;
    if (flags & MAP_ANONYMOUS) {
        if (!(flags & MAP_PRIVATE))
            return -EINVAL;
        return vma_mmap(vm, addr, ALIGN_UP(len, PAGE_SIZE), vmflags);
    }
    struct file *f = fdtable_get(&thread_current()->proc->fds, fd);
    if (!f)
        return -EBADF;
    long r = -ENODEV;
    if (f->ops && f->ops->mmap)
        r = f->ops->mmap(f, vm, addr, ALIGN_UP(len, PAGE_SIZE), vmflags, off);
    file_put(f);
    return r;
}

long sys_munmap(struct trapframe *tf)
{
    return vma_munmap(thread_current()->proc->vm, SYSARG0(tf), SYSARG1(tf));
}
