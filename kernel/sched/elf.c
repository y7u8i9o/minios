#define KLOG_SUBSYS "elf"
#include <sched/elf.h>
#include <mm/vmm.h>
#include <mm/vma.h>
#include <mm/memlayout.h>
#include <lib/string.h>
#include <mm/slab.h>
#include <klog.h>
#include <errno.h>

#define ELFMAG      "\x7f""ELF"
#define ELFCLASS64  2
#define ELFDATA2LSB 1
#define ET_EXEC     2
#define EM_X86_64   62
#define PT_LOAD     1
#define PF_X        1
#define PF_W        2
#define PF_R        4

struct elf64_ehdr {
    uint8_t e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
};

struct elf64_phdr {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
};

/* Copy bytes into a populated user region through the page tables. */
static int write_user(struct vmspace *vm, uintptr_t va, const void *src, size_t n)
{
    const uint8_t *s = src;
    while (n) {
        uintptr_t pa;
        if (!vmm_translate(vm, va, &pa, NULL))
            return -EFAULT;
        size_t chunk = MIN(n, PAGE_SIZE - (va & (PAGE_SIZE - 1)));
        memcpy(P2V(pa), s, chunk);
        va += chunk;
        s += chunk;
        n -= chunk;
    }
    return 0;
}

int elf_load(struct vmspace *vm, const void *image, size_t size, uintptr_t *entry)
{
    const struct elf64_ehdr *eh = image;
    if (size < sizeof *eh || memcmp(eh->e_ident, ELFMAG, 4) != 0 ||
        eh->e_ident[4] != ELFCLASS64 || eh->e_ident[5] != ELFDATA2LSB ||
        eh->e_type != ET_EXEC || eh->e_machine != EM_X86_64)
        return -ENOEXEC;
    if (eh->e_phentsize != sizeof(struct elf64_phdr) ||
        eh->e_phoff + (uint64_t)eh->e_phnum * sizeof(struct elf64_phdr) > size)
        return -ENOEXEC;

    uintptr_t highest = 0;
    for (int i = 0; i < eh->e_phnum; i++) {
        const struct elf64_phdr *ph = (const struct elf64_phdr *)((const uint8_t *)image + eh->e_phoff) + i;
        if (ph->p_type != PT_LOAD || ph->p_memsz == 0)
            continue;
        if (ph->p_offset + ph->p_filesz > size || ph->p_filesz > ph->p_memsz)
            return -ENOEXEC;
        if (ph->p_vaddr < USER_BASE || ph->p_vaddr + ph->p_memsz - 1 > USER_TOP)
            return -ENOEXEC;
        unsigned flags = 0;
        if (ph->p_flags & PF_R)
            flags |= VM_READ;
        if (ph->p_flags & PF_W)
            flags |= VM_WRITE;
        if (ph->p_flags & PF_X)
            flags |= VM_EXEC;
        uintptr_t start = ALIGN_DOWN(ph->p_vaddr, PAGE_SIZE);
        uintptr_t end = ALIGN_UP(ph->p_vaddr + ph->p_memsz, PAGE_SIZE);
        int r = vma_add(vm, start, end, flags | VM_WRITE);   /* writable while loading */
        if (r < 0)
            return r;
        r = vma_populate(vm, start, end);
        if (r < 0)
            return r;
        r = write_user(vm, ph->p_vaddr, (const uint8_t *)image + ph->p_offset, ph->p_filesz);
        if (r < 0)
            return r;
        if (!(flags & VM_WRITE)) {
            spin_lock(&vm->lock);
            struct vma *v = vma_find_locked(vm, start);
            v->flags = flags;
            spin_unlock(&vm->lock);
            vmm_protect(vm, start, end - start, flags | VM_USER);
        }
        if (end > highest)
            highest = end;
    }
    if (!highest)
        return -ENOEXEC;

    /* Heap: one page to start, grown by brk. */
    uintptr_t heap = highest + PAGE_SIZE;
    int r = vma_add(vm, heap, heap + PAGE_SIZE, VM_READ | VM_WRITE);
    if (r < 0)
        return r;
    spin_lock(&vm->lock);
    vm->brk_start = heap;
    vm->brk = heap + PAGE_SIZE;
    spin_unlock(&vm->lock);
    *entry = eh->e_entry;
    return 0;
}

static size_t count_strings(char *const list[])
{
    size_t n = 0;
    if (list)
        while (list[n])
            n++;
    return n;
}

int user_stack_setup(struct vmspace *vm, char *const argv[], char *const envp[], uintptr_t *rsp,
                     size_t stack_size)
{
    uintptr_t bottom = USER_STACK_TOP - stack_size;
    int r = vma_add(vm, bottom, USER_STACK_TOP, VM_READ | VM_WRITE);
    if (r < 0)
        return r;

    size_t argc = count_strings(argv);
    size_t envc = count_strings(envp);
    size_t bytes = 0;
    for (size_t i = 0; i < argc; i++)
        bytes += strlen(argv[i]) + 1;
    for (size_t i = 0; i < envc; i++)
        bytes += strlen(envp[i]) + 1;
    size_t vectors = (1 + argc + 1 + envc + 1) * sizeof(uint64_t);
    if (bytes + vectors + 64 > stack_size / 2)
        return -E2BIG;

    /* Strings at the top, then the pointer vectors below, 16 byte aligned
     * so that rsp % 16 == 0 at the entry point with argc on top. */
    uintptr_t sp = USER_STACK_TOP - bytes;
    uintptr_t strings = sp;
    sp = ALIGN_DOWN(sp - vectors, 16);
    if (((sp + vectors) & 15) == 8)
        sp -= 8;
    r = vma_populate(vm, sp, USER_STACK_TOP);
    if (r < 0)
        return r;

    uint64_t *vec = (uint64_t *)kmalloc(vectors);
    if (!vec)
        return -ENOMEM;
    size_t idx = 0;
    vec[idx++] = argc;
    uintptr_t p = strings;
    for (size_t i = 0; i < argc; i++) {
        size_t n = strlen(argv[i]) + 1;
        write_user(vm, p, argv[i], n);
        vec[idx++] = p;
        p += n;
    }
    vec[idx++] = 0;
    for (size_t i = 0; i < envc; i++) {
        size_t n = strlen(envp[i]) + 1;
        write_user(vm, p, envp[i], n);
        vec[idx++] = p;
        p += n;
    }
    vec[idx++] = 0;
    write_user(vm, sp, vec, vectors);
    kfree(vec);
    *rsp = sp;
    return 0;
}
