#include <syscall/syscalls.h>
#include <arch/trap.h>
#include <sched/thread.h>
#include <sched/proc.h>
#include <mm/vma.h>
#include <mm/filemap.h>
#include <fs/vfs.h>
#include <fs/fdtable.h>
#include <ipc/mqueue.h>
#include <minios/abi.h>
#include <errno.h>

long sys_sbrk(struct trapframe *tf)
{
    return vma_brk(thread_current()->proc->vm, (intptr_t)SYSARG0(tf));
}

static unsigned prot_to_vmflags(int prot)
{
    unsigned vmflags = 0;
    if (prot & (PROT_READ | PROT_WRITE | PROT_EXEC))
        vmflags |= VM_READ;
    if (prot & PROT_WRITE)
        vmflags |= VM_WRITE;
    if (prot & PROT_EXEC)
        vmflags |= VM_EXEC;
    return vmflags;
}

/* Map a regular file: the region references the file and its mapping. */
static long mmap_regular(struct file *f, struct vmspace *vm, uintptr_t addr, size_t len, unsigned vmflags,
                         bool fixed, uint64_t off)
{
    if (!IS_ALIGNED(off, PAGE_SIZE) || off + len < off)
        return -EINVAL;
    if (!f->ops || !f->ops->read)
        return -ENODEV;
    int acc = f->flags & O_ACCMODE;
    if (acc == O_WRONLY)
        return -EACCES;
    if ((vmflags & VM_SHARED) && (vmflags & VM_WRITE) &&
        (acc != O_RDWR || (f->flags & O_APPEND) || !f->ops->write))
        return -EACCES;
    struct mapping *m = filemap_get(f->inode);
    if (!m)
        return -ENOMEM;
    file_ref(f);
    long va = vma_mmap_file(vm, addr, len, vmflags | VM_FILE, fixed, f, m, off);
    if (va < 0) {
        file_put(f);
        filemap_put(m);
    }
    return va;
}

/* Anonymous shared memory is an unnamed shared memory object mapped once;
 * fork then shares the frames. */
static long mmap_anon_shared(struct vmspace *vm, uintptr_t addr, size_t len, unsigned vmflags)
{
    struct file *f;
    int r = shm_create_anon(&f);
    if (r < 0)
        return r;
    r = f->ops->truncate(f, len);
    long va = r < 0 ? r : f->ops->mmap(f, vm, addr, len, vmflags, 0);
    file_put(f);
    return va;
}

/* mmap(addr, len, prot, flags, fd, off) */
long sys_mmap(struct trapframe *tf)
{
    uintptr_t addr = SYSARG0(tf);
    size_t len = SYSARG1(tf);
    int prot = (int)SYSARG2(tf);
    int flags = (int)SYSARG3(tf);
    int fd = (int)SYSARG4(tf);
    uint64_t off = SYSARG5(tf);
    if (len == 0 || ALIGN_UP(len, PAGE_SIZE) < len)
        return -EINVAL;
    len = ALIGN_UP(len, PAGE_SIZE);
    bool shared = flags & MAP_SHARED;
    if (shared == !!(flags & MAP_PRIVATE))
        return -EINVAL;
    unsigned vmflags = prot_to_vmflags(prot);
    if (shared)
        vmflags |= VM_SHARED;
    struct vmspace *vm = thread_current()->proc->vm;
    bool fixed = flags & MAP_FIXED;
    if (fixed) {
        if (!IS_ALIGNED(addr, PAGE_SIZE) || addr < USER_BASE || addr + len - 1 > USER_TOP || addr + len < addr)
            return -EINVAL;
        if (!vma_range_replaceable(vm, addr, len))
            return -EINVAL;
        int r = vma_munmap(vm, addr, len);
        if (r < 0)
            return r;
    }
    if (flags & MAP_ANONYMOUS) {
        if (shared)
            return mmap_anon_shared(vm, addr, len, vmflags);
        return vma_mmap_file(vm, addr, len, vmflags, fixed, NULL, NULL, 0);
    }
    struct file *f = fdtable_get(&thread_current()->proc->fds, fd);
    if (!f)
        return -EBADF;
    long r;
    if (f->ops && f->ops->mmap)
        r = f->ops->mmap(f, vm, addr, len, vmflags, off);
    else if (f->inode && S_ISREG(f->inode->mode))
        r = mmap_regular(f, vm, addr, len, vmflags, fixed, off);
    else
        r = -ENODEV;
    file_put(f);
    return r;
}

long sys_munmap(struct trapframe *tf)
{
    return vma_munmap(thread_current()->proc->vm, SYSARG0(tf), SYSARG1(tf));
}

long sys_mprotect(struct trapframe *tf)
{
    int prot = (int)SYSARG2(tf);
    if (prot & ~(PROT_READ | PROT_WRITE | PROT_EXEC))
        return -EINVAL;
    return vma_mprotect(thread_current()->proc->vm, SYSARG0(tf), SYSARG1(tf), prot_to_vmflags(prot));
}

long sys_msync(struct trapframe *tf)
{
    int flags = (int)SYSARG2(tf);
    if ((flags & ~(MS_ASYNC | MS_SYNC | MS_INVALIDATE)) || ((flags & MS_ASYNC) && (flags & MS_SYNC)))
        return -EINVAL;
    return vma_msync(thread_current()->proc->vm, SYSARG0(tf), SYSARG1(tf));
}

long sys_madvise(struct trapframe *tf)
{
    return vma_madvise(thread_current()->proc->vm, SYSARG0(tf), SYSARG1(tf), (int)SYSARG2(tf));
}
