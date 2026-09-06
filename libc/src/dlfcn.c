#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdint.h>

/* One mapped library: its base and the dynamic symbols of its file, read
 * once. The list is built from /dev/maps on the first call. */
struct library {
    char *path;
    uint64_t base;
    struct elf_sym *syms;
    size_t nsyms;
    char *strings;
    size_t strsz;
    int loaded;
    struct library *next;
};

struct elf_sym {
    uint32_t st_name;
    uint8_t st_info, st_other;
    uint16_t st_shndx;
    uint64_t st_value, st_size;
};

struct elf_shdr {
    uint32_t sh_name, sh_type;
    uint64_t sh_flags, sh_addr, sh_offset, sh_size;
    uint32_t sh_link, sh_info;
    uint64_t sh_addralign, sh_entsize;
};

#define SHT_DYNSYM 11

static struct library *libraries;
static int scanned;
static char error_text[256];
static int error_set;

static void set_error(const char *what, const char *name)
{
    snprintf(error_text, sizeof error_text, "%s: %s", what, name);
    error_set = 1;
}

/* Every /lib file mapped into this process, with its base address: the
 * lowest region start minus its file offset. */
static void scan_maps(void)
{
    if (scanned)
        return;
    scanned = 1;
    FILE *f = fopen("/dev/maps", "r");
    if (f == NULL)
        return;
    int me = getpid();
    char line[512];
    while (fgets(line, sizeof line, f)) {
        int pid;
        unsigned long start, end, offset;
        char path[256];
        if (sscanf(line, "%d %lx %lx %lx %255s", &pid, &start, &end, &offset, path) != 5 || pid != me)
            continue;
        if (strncmp(path, "/lib/", 5) != 0)
            continue;
        struct library *l;
        for (l = libraries; l != NULL; l = l->next)
            if (strcmp(l->path, path) == 0)
                break;
        if (l == NULL) {
            l = calloc(1, sizeof *l);
            if (l == NULL)
                return;
            l->path = strdup(path);
            l->base = start - offset;
            l->next = libraries;
            libraries = l;
        } else if (start - offset < l->base) {
            l->base = start - offset;
        }
    }
    fclose(f);
}

static int read_at(int fd, long off, void *buf, size_t n)
{
    if (lseek(fd, off, SEEK_SET) < 0)
        return -1;
    while (n > 0) {
        ssize_t r = read(fd, buf, n);
        if (r <= 0)
            return -1;
        buf = (char *)buf + r;
        n -= (size_t)r;
    }
    return 0;
}

/* Read the .dynsym and .dynstr sections of a library file. */
static void load_symbols(struct library *l)
{
    if (l->loaded)
        return;
    l->loaded = 1;
    int fd = open(l->path, O_RDONLY);
    if (fd < 0)
        return;
    unsigned char eh[64];
    if (read_at(fd, 0, eh, sizeof eh) < 0 || memcmp(eh, "\177ELF", 4) != 0) {
        close(fd);
        return;
    }
    uint64_t shoff;
    uint16_t shnum, shentsize;
    memcpy(&shoff, eh + 40, 8);
    memcpy(&shentsize, eh + 58, 2);
    memcpy(&shnum, eh + 60, 2);
    if (shentsize != sizeof(struct elf_shdr)) {
        close(fd);
        return;
    }
    struct elf_shdr *sh = malloc((size_t)shnum * sizeof *sh);
    if (sh == NULL || read_at(fd, (long)shoff, sh, (size_t)shnum * sizeof *sh) < 0) {
        free(sh);
        close(fd);
        return;
    }
    for (int i = 0; i < shnum; i++) {
        if (sh[i].sh_type != SHT_DYNSYM || sh[i].sh_link >= shnum)
            continue;
        const struct elf_shdr *strsh = &sh[sh[i].sh_link];
        l->syms = malloc(sh[i].sh_size);
        l->strings = malloc(strsh->sh_size + 1);
        if (l->syms == NULL || l->strings == NULL ||
            read_at(fd, (long)sh[i].sh_offset, l->syms, sh[i].sh_size) < 0 ||
            read_at(fd, (long)strsh->sh_offset, l->strings, strsh->sh_size) < 0) {
            free(l->syms);
            free(l->strings);
            l->syms = NULL;
            l->strings = NULL;
            break;
        }
        l->nsyms = sh[i].sh_size / sizeof(struct elf_sym);
        l->strsz = strsh->sh_size;
        l->strings[strsh->sh_size] = '\0';
        break;
    }
    free(sh);
    close(fd);
}

static void *lookup(struct library *l, const char *name)
{
    load_symbols(l);
    for (size_t i = 0; i < l->nsyms; i++) {
        const struct elf_sym *s = &l->syms[i];
        if (s->st_shndx == 0 || s->st_name >= l->strsz)
            continue;
        if (strcmp(l->strings + s->st_name, name) == 0)
            return (void *)(uintptr_t)(l->base + s->st_value);
    }
    return NULL;
}

void *dlopen(const char *file, int flags)
{
    scan_maps();
    if (file == NULL)
        return RTLD_DEFAULT;
    const char *base = strrchr(file, '/');
    base = base != NULL ? base + 1 : file;
    for (struct library *l = libraries; l != NULL; l = l->next)
        if (strcmp(l->path + 5, base) == 0)
            return l;
    set_error("library is not mapped in this process", file);
    return NULL;
}

void *dlsym(void *handle, const char *name)
{
    scan_maps();
    if (handle == RTLD_DEFAULT) {
        for (struct library *l = libraries; l != NULL; l = l->next) {
            void *p = lookup(l, name);
            if (p != NULL)
                return p;
        }
    } else {
        void *p = lookup(handle, name);
        if (p != NULL)
            return p;
    }
    set_error("undefined symbol", name);
    return NULL;
}

int dlclose(void *handle)
{
    return 0;
}

char *dlerror(void)
{
    if (!error_set)
        return NULL;
    error_set = 0;
    return error_text;
}
