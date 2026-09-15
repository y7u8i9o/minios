/* Thread local storage (docs/design/dynlink.md). The loader lays out
 * the blocks of the objects loaded at start below every thread control
 * block and serves the objects loaded by dlopen; a static program has
 * one module, its own PT_TLS segment, handled here. __tls_get_addr is
 * what compiled code calls for the general dynamic model. */
#include "tcb.h"
#include <minios/dl.h>
#include <string.h>

struct dl_interface *__dl_interface;

/* The program's segment when there is no loader. */
static struct dl_tls_module program_module;
static size_t static_size, static_align = 16;

struct elf_phdr {
    uint32_t p_type, p_flags;
    uint64_t p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align;
};
#define PT_TLS 7
#define AT_PHDR 3
#define AT_PHNUM 5

#define ALIGN_UP(x, a) (((x) + (a) - 1) & ~((a) - 1))

/* Called first at startup with the auxiliary vector. */
void __tls_init(const uintptr_t *aux)
{
    if (__dl_interface) {
        static_size = __dl_interface->static_tls_size;
        static_align = __dl_interface->static_tls_align;
        return;
    }
    const struct elf_phdr *phdr = NULL;
    size_t phnum = 0;
    for (; aux[0]; aux += 2) {
        if (aux[0] == AT_PHDR)
            phdr = (const void *)aux[1];
        if (aux[0] == AT_PHNUM)
            phnum = aux[1];
    }
    for (size_t i = 0; phdr && i < phnum; i++) {
        if (phdr[i].p_type != PT_TLS || !phdr[i].p_memsz)
            continue;
        program_module.image = phdr[i].p_vaddr;
        program_module.filesz = phdr[i].p_filesz;
        program_module.memsz = phdr[i].p_memsz;
        program_module.align = phdr[i].p_align > 1 ? phdr[i].p_align : 1;
        program_module.offset = ALIGN_UP(program_module.memsz, program_module.align);
        static_size = program_module.offset;
        if (program_module.align > static_align)
            static_align = program_module.align;
        break;
    }
}

/* Bytes to reserve below a control block, and the alignment the block
 * itself needs. */
size_t __tls_reserve(size_t *align)
{
    *align = static_align;
    return ALIGN_UP(static_size, static_align);
}

void __tls_setup(struct pthread *t)
{
    t->dtv = NULL;
    if (__dl_interface) {
        __dl_interface->tls_setup(t);
    } else if (program_module.offset) {
        unsigned char *block = (unsigned char *)t - program_module.offset;
        memcpy(block, (const void *)program_module.image, program_module.filesz);
        memset(block + program_module.filesz, 0, program_module.memsz - program_module.filesz);
    }
}

void __tls_free(struct pthread *t)
{
    if (__dl_interface)
        __dl_interface->tls_free(t);
}

void *__tls_get_addr(struct dl_tls_index *index)
{
    struct dl_tls_module *m = (struct dl_tls_module *)index->module;
    if (m->offset)
        return (unsigned char *)__pthread_current() - m->offset + index->offset;
    return __dl_interface->tls_get_addr(m, index->offset);
}
