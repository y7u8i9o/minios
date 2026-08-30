/*
 * gensyms: convert `nm -n -S` output on stdin into the kernel symbol blob.
 *
 * Blob layout (all little endian, native on x86_64):
 *   struct ksyms_header { u32 magic; u32 count; u32 strtab_off; u32 strtab_len; }
 *   struct ksyms_entry  { u64 addr; u32 size; u32 name_off; } [count], sorted by addr
 *   char strtab[strtab_len]
 * Only text symbols (t, T, W, w) are kept.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define KSYMS_MAGIC 0x53594d4bu /* "KMYS" */

struct sym {
    uint64_t addr;
    uint32_t size;
    uint32_t name_off;
};

static struct sym *syms;
static size_t nsyms, capsyms;
static char *strtab;
static size_t strlen_total, strcap;

static void add_sym(uint64_t addr, uint32_t size, const char *name)
{
    size_t len = strlen(name) + 1;
    if (nsyms == capsyms) {
        capsyms = capsyms ? capsyms * 2 : 1024;
        syms = realloc(syms, capsyms * sizeof(*syms));
        if (!syms) { perror("realloc"); exit(1); }
    }
    if (strlen_total + len > strcap) {
        strcap = (strcap ? strcap * 2 : 65536) + len;
        strtab = realloc(strtab, strcap);
        if (!strtab) { perror("realloc"); exit(1); }
    }
    memcpy(strtab + strlen_total, name, len);
    syms[nsyms].addr = addr;
    syms[nsyms].size = size;
    syms[nsyms].name_off = (uint32_t)strlen_total;
    nsyms++;
    strlen_total += len;
}

static int cmp_sym(const void *a, const void *b)
{
    const struct sym *x = a, *y = b;
    if (x->addr < y->addr) return -1;
    if (x->addr > y->addr) return 1;
    return 0;
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: nm -n -S kernel.elf | gensyms <out.bin>\n");
        return 1;
    }
    char line[4096];
    while (fgets(line, sizeof line, stdin)) {
        char f1[64], f2[64], f3[64], f4[1024];
        int n = sscanf(line, "%63s %63s %63s %1023s", f1, f2, f3, f4);
        uint64_t addr;
        uint32_t size = 0;
        char type;
        const char *name;
        if (n == 4 && strlen(f3) == 1) {
            addr = strtoull(f1, NULL, 16);
            size = (uint32_t)strtoull(f2, NULL, 16);
            type = f3[0];
            name = f4;
        } else if (n == 3 && strlen(f2) == 1) {
            addr = strtoull(f1, NULL, 16);
            type = f2[0];
            name = f3;
        } else {
            continue;
        }
        if (type != 't' && type != 'T' && type != 'W' && type != 'w')
            continue;
        if (strncmp(name, ".L", 2) == 0)
            continue;
        add_sym(addr, size, name);
    }
    qsort(syms, nsyms, sizeof(*syms), cmp_sym);

    FILE *out = fopen(argv[1], "wb");
    if (!out) { perror(argv[1]); return 1; }
    uint32_t hdr[4];
    hdr[0] = KSYMS_MAGIC;
    hdr[1] = (uint32_t)nsyms;
    hdr[2] = (uint32_t)(sizeof hdr + nsyms * 16);
    hdr[3] = (uint32_t)strlen_total;
    fwrite(hdr, sizeof hdr, 1, out);
    for (size_t i = 0; i < nsyms; i++) {
        fwrite(&syms[i].addr, 8, 1, out);
        fwrite(&syms[i].size, 4, 1, out);
        fwrite(&syms[i].name_off, 4, 1, out);
    }
    fwrite(strtab, 1, strlen_total, out);
    if (fclose(out) != 0) { perror("fclose"); return 1; }
    return 0;
}
