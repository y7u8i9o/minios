#pragma once
/* Sampling profiler client (M41): /dev/profile control, sample reading,
 * symbol tables of static ELF binaries and of the kernel (/dev/ksyms), and
 * a histogram of symbols. */
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <minios/abi.h>

/* ---- /dev/profile ---- */
int prof_open(void);                        /* descriptor or -1 */
int prof_start(int fd, pid_t pid);          /* pid 0: every process */
int prof_stop(int fd);
int prof_set_divider(int fd, unsigned ticks);
int prof_get_stats(int fd, struct prof_stats *st);
/* Read up to max samples; returns the count, 0 when none are pending. */
ssize_t prof_read(int fd, struct prof_sample *buf, size_t max);

/* ---- symbol tables ---- */
struct prof_sym {
    uint64_t addr;
    uint64_t size;
    const char *name;
};
struct prof_symtab {
    struct prof_sym *syms;      /* sorted by address */
    size_t count;
    char *strings;
};
/* The function symbols of a static ELF file, or NULL. */
struct prof_symtab *prof_symtab_load_elf(const char *path);
/* The kernel's symbols from /dev/ksyms, or NULL. */
struct prof_symtab *prof_symtab_load_kernel(void);
void prof_symtab_free(struct prof_symtab *t);
/* The symbol containing addr, or NULL; *off receives the offset inside. */
const struct prof_sym *prof_symtab_lookup(const struct prof_symtab *t, uint64_t addr, uint64_t *off);

/* ---- histogram ---- */
struct prof_bucket {
    char *key;                  /* symbol name or a chain "a;b;c" */
    unsigned count;
    unsigned kernel;            /* samples of the key taken in kernel mode */
};
struct prof_hist {
    struct prof_bucket *buckets;
    size_t count, cap;
    unsigned total;
};
void prof_hist_init(struct prof_hist *h);
void prof_hist_clear(struct prof_hist *h);
void prof_hist_add(struct prof_hist *h, const char *key, int kernel);
/* Sort buckets by descending count. */
void prof_hist_sort(struct prof_hist *h);
/* Attribute a sample to a symbol. A kernel sample whose innermost frames
 * are the lock primitives (pop_cli, spin_unlock, ...) is attributed to the
 * first frame outside them, the code that held the lock, and *locked is
 * set; the timer interrupt lands in pop_cli when it was pending while
 * interrupts were disabled. */
void prof_attribute(const struct prof_symtab *user, const struct prof_symtab *kernel, const struct prof_sample *s,
                    int *locked, char *buf, size_t size);
/* Format an address through user and kernel tables: "name+0xoff" or the
 * hexadecimal address. */
void prof_format_addr(const struct prof_symtab *user, const struct prof_symtab *kernel, int is_kernel,
                      uint64_t addr, int with_offset, char *buf, size_t size);
