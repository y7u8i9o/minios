#define KLOG_SUBSYS "user"
#include <sched/user.h>
#include <arch/thread.h>
#include <sched/proc.h>
#include <sched/thread.h>
#include <sched/sched.h>
#include <sched/elf.h>
#include <fs/vfs.h>
#include <fs/fdtable.h>
#include <arch/frame.h>
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

/* Build a new address space from an ELF on the initrd. */
/* The main stack size for a new image: RLIMIT_STACK of the creating
 * process, limited to between 64 KiB and 1 GiB and page aligned. */
static size_t stack_size_for(struct proc *p)
{
    uint64_t lim = p ? proc_rlimit_cur(p, RLIMIT_STACK) : 8UL << 20;
    if (lim == RLIM_INFINITY || lim > (1UL << 30))
        lim = 1UL << 30;
    if (lim < USER_STACK_MIN)
        lim = USER_STACK_MIN;
    return ALIGN_UP(lim, PAGE_SIZE);
}

/* The set id bits of a program and the ids they confer (U2). */
struct exec_ids {
    bool setuid, setgid;
    uint32_t uid, gid;
};

/* The longest #! line of a script, its newline included (R1). */
#define SCRIPT_LINE_MAX 256
/* The deepest chain of scripts whose interpreter is a script again. */
#define SCRIPT_DEPTH_MAX 4

/* Read a whole regular file with execute permission into kernel memory,
 * and report its set id bits when ids is not NULL. The read of a script
 * ends after its first SCRIPT_LINE_MAX bytes, since only its #! line is
 * needed. */
static int read_file_image(const char *path, void **image_out, size_t *size_out, struct exec_ids *ids)
{
    struct file *f;
    int r = vfs_open_exec(path, &f);
    if (r < 0)
        return r;
    if (ids) {
        mutex_lock(&f->inode->lock);
        ids->setuid = f->inode->mode & S_ISUID;
        ids->setgid = f->inode->mode & S_ISGID;
        ids->uid = f->inode->uid;
        ids->gid = f->inode->gid;
        mutex_unlock(&f->inode->lock);
    }
    size_t size = f->inode->size;
    void *image = kmalloc(size ? size : 1);
    if (!image) {
        file_put(f);
        return -ENOMEM;
    }
    size_t got = 0;
    while (got < size) {
        size_t want = size - got;
        if (got < SCRIPT_LINE_MAX && want > SCRIPT_LINE_MAX - got)
            want = SCRIPT_LINE_MAX - got;
        long n = file_read(f, (char *)image + got, want);
        if (n <= 0) {
            r = n < 0 ? (int)n : -EIO;
            break;
        }
        got += (size_t)n;
        if (got >= 2 && got <= SCRIPT_LINE_MAX && memcmp(image, "#!", 2) == 0 &&
            (got == SCRIPT_LINE_MAX || got == size)) {
            size = got;
            break;
        }
    }
    file_put(f);
    if (r < 0) {
        kfree(image);
        return r;
    }
    *image_out = image;
    *size_out = size;
    return 0;
}

/* The #! line of a script: the interpreter and the optional argument,
 * both pointers into line. */
struct script_line {
    char line[SCRIPT_LINE_MAX + 1];
    char *interp;
    char *arg;
};

static bool script_blank(char c)
{
    return c == ' ' || c == '\t';
}

/* Parse the #! line at the start of image. The interpreter ends at the
 * first blank. The rest of the line without its surrounding blanks is the
 * argument, which may contain blanks. A line that does not end within
 * SCRIPT_LINE_MAX bytes before the end of the file is refused, since its
 * interpreter could be cut. */
static int script_parse(const char *image, size_t size, struct script_line *s)
{
    size_t len = 2;
    while (len < size && image[len] != '\n')
        len++;
    if (len == SCRIPT_LINE_MAX)
        return -ENOEXEC;
    memcpy(s->line, image, len);
    s->line[len] = '\0';
    char *p = s->line + 2;
    while (script_blank(*p))
        p++;
    if (!*p)
        return -ENOEXEC;
    s->interp = p;
    while (*p && !script_blank(*p))
        p++;
    s->arg = NULL;
    if (*p) {
        *p++ = '\0';
        while (script_blank(*p))
            p++;
        char *end = p + strlen(p);
        while (end > p && (script_blank(end[-1]) || end[-1] == '\r'))
            *--end = '\0';
        if (*p)
            s->arg = p;
    } else if (p > s->interp && p[-1] == '\r') {
        p[-1] = '\0';
    }
    return 0;
}

static int load_image_depth(const char *path, char *const argv[], char *const envp[],
                            struct vmspace **vm_out, uintptr_t *entry, uintptr_t *rsp, size_t stack_size,
                            char *interp, size_t interp_len, struct exec_ids *ids, int depth);

/* Start the interpreter of a script with the argument vector interpreter,
 * optional argument, path of the script, and the original arguments after
 * the first. The set id bits of the script are ignored. The bits of the
 * interpreter apply, as for any program. */
static int load_script(const char *path, const char *image, size_t size, char *const argv[], char *const envp[],
                       struct vmspace **vm_out, uintptr_t *entry, uintptr_t *rsp, size_t stack_size,
                       char *interp, size_t interp_len, struct exec_ids *ids, int depth)
{
    if (depth >= SCRIPT_DEPTH_MAX)
        return -ELOOP;
    struct script_line *s = kmalloc(sizeof *s);
    if (!s)
        return -ENOMEM;
    int r = script_parse(image, size, s);
    if (r < 0) {
        kfree(s);
        return r;
    }
    int argc = 0;
    while (argv && argv[argc])
        argc++;
    char **nargv = kmalloc((size_t)(argc + 4) * sizeof *nargv);
    if (!nargv) {
        kfree(s);
        return -ENOMEM;
    }
    int n = 0;
    nargv[n++] = s->interp;
    if (s->arg)
        nargv[n++] = s->arg;
    nargv[n++] = (char *)path;
    for (int i = 1; i < argc; i++)
        nargv[n++] = argv[i];
    nargv[n] = NULL;
    if (ids)
        *ids = (struct exec_ids){ 0 };
    r = load_image_depth(s->interp, nargv, envp, vm_out, entry, rsp, stack_size, interp, interp_len, ids,
                         depth + 1);
    kfree(nargv);
    kfree(s);
    return r;
}

/* Build the address space of a program: its segments, the loader named by
 * PT_INTERP at USER_INTERP_BASE when the program is dynamically linked, and
 * the initial stack. The thread starts in the loader in that case. A file
 * that begins with #! is a script, whose interpreter is loaded instead. */
static int load_image(const char *path, char *const argv[], char *const envp[],
                      struct vmspace **vm_out, uintptr_t *entry, uintptr_t *rsp, size_t stack_size,
                      char *interp, size_t interp_len, struct exec_ids *ids)
{
    return load_image_depth(path, argv, envp, vm_out, entry, rsp, stack_size, interp, interp_len, ids, 0);
}

static int load_image_depth(const char *path, char *const argv[], char *const envp[],
                            struct vmspace **vm_out, uintptr_t *entry, uintptr_t *rsp, size_t stack_size,
                            char *interp, size_t interp_len, struct exec_ids *ids, int depth)
{
    void *image;
    size_t size;
    struct exec_ids none;
    int r = read_file_image(path, &image, &size, ids ? ids : &none);
    if (r < 0)
        return r;
    if (size >= 2 && memcmp(image, "#!", 2) == 0) {
        r = load_script(path, image, size, argv, envp, vm_out, entry, rsp, stack_size, interp, interp_len, ids,
                        depth);
        kfree(image);
        return r;
    }
    struct vmspace *vm = vmspace_create();
    if (!vm) {
        kfree(image);
        return -ENOMEM;
    }
    struct elf_info info = { 0 };
    r = elf_load(vm, image, size, &info);
    kfree(image);
    /* A program that changes the ids gets AT_SECURE, which tells the loader
     * and libc not to trust the environment. elf_load cleared info. */
    info.secure = ids && (ids->setuid || ids->setgid);
    if (r == 0 && info.interp[0]) {
        r = read_file_image(info.interp, &image, &size, NULL);
        if (r == 0) {
            r = elf_load_interp(vm, image, size, USER_INTERP_BASE, &info);
            kfree(image);
        }
        if (r < 0)
            klog_error("%s: loader %s: error %d", path, info.interp, r);
    }
    if (r == 0)
        r = user_stack_setup(vm, argv, envp, rsp, stack_size, &info);
    kfree(info.phdr_copy);
    if (r < 0) {
        vma_remove_all(vm);
        vmspace_destroy(vm);
        return r;
    }
    *entry = info.interp[0] ? info.interp_entry : info.entry;
    if (interp)
        strlcpy(interp, info.interp, interp_len);
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
        arch_set_tls(t, arch_get_tls(thread_current()));
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
    char interp[64];
    int r = load_image(path, argv, envp, &vm, &entry, &rsp, stack_size_for(parent), interp, sizeof interp, NULL);
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
    arch_frame_init_user(tf, entry, rsp);
    if (!start_thread(p, base, tf)) {
        kfree(tf);
        goto fail_proc;
    }
    if (interp[0])
        klog_info("process %s (pid %d) from %s, dynamic, %s entry %lx", p->name, p->pid, path,
                  interp, entry);
    else
        klog_info("process %s (pid %d) from %s, static, entry %lx", p->name, p->pid, path, entry);
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
    child->sid = parent->sid;
    spin_unlock(&proc_tree_lock);
    struct trapframe *ctf = kmalloc(sizeof *ctf);
    if (!ctf)
        goto fail;
    *ctf = *tf;
    frame_set_retval(ctf, 0);
    struct thread *t = prepare_thread(child, cur->name, ctf);
    if (!t) {
        kfree(ctf);
        goto fail;
    }
    /* The child starts with the parent's registers. Its FPU image must be
     * complete before it can run, so it is queued only afterwards. */
    arch_fpu_capture(t);
    arch_set_tls(t, arch_get_tls(cur));
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
    struct exec_ids ids = { 0 };
    int r = load_image(path, argv, envp, &vm, &entry, &rsp, stack_size_for(p), NULL, 0, &ids);
    if (r < 0)
        return r;
    /* The set user id and set group id bits make the owner and the group
     * of the program the effective and saved ids. */
    if (ids.setuid || ids.setgid) {
        spin_lock(&p->lock);
        if (ids.setuid)
            p->cred.euid = p->cred.suid = ids.uid;
        if (ids.setgid)
            p->cred.egid = p->cred.sgid = ids.gid;
        spin_unlock(&p->lock);
    }

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
    arch_fpu_reset(cur);
    arch_set_tls(cur, 0);               /* the new image sets up its own thread local storage */
    arch_frame_init_user(tf, entry, rsp);
    return 0;
}

int user_thread_create(uintptr_t entry, uintptr_t arg, uintptr_t stack)
{
    struct thread *cur = thread_current();
    struct trapframe *tf = kmalloc(sizeof *tf);
    if (!tf)
        return -ENOMEM;
    arch_frame_init_user(tf, entry, stack);
    frame_set_arg0(tf, arg);
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
