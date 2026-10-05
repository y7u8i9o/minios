/* Memory that a process does not use is not resident (docs/design/process.md,
 * docs/design/libc.md). The program has 8 MiB of bss, which exec maps
 * without touching it, so the resident size at the start stays small.
 * 20 MiB of small blocks from malloc become resident when they are
 * written. Freed with a block at the top of the heap that remains in use,
 * the merged free block inside the heap gives its pages back with
 * madvise. Freed with the top block as well, the heap shrinks. The memory
 * is then allocated again and must read as zero through calloc. Exits 0
 * on success. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

#define NBLOCKS 20000
#define BLOCK 1000

static char bss[8 << 20];
static char *blocks[NBLOCKS];

/* The resident size of this process in KiB, from the RSS column of
 * /dev/proc: PID PPID PGID STATE TIME RSS UID NAME. */
static long rss_kib(void)
{
    static char table[16384];
    int fd = open("/dev/proc", O_RDONLY);
    if (fd < 0)
        return -1;
    ssize_t n = read(fd, table, sizeof table - 1);
    close(fd);
    if (n <= 0)
        return -1;
    table[n] = '\0';
    int self = getpid();
    for (char *line = table; line && *line;) {
        char *end = strchr(line, '\n');
        long pid, ppid, pgid, ticks, rss;
        char state[16];
        if (sscanf(line, "%ld %ld %ld %15s %ld %ld", &pid, &ppid, &pgid, state, &ticks, &rss) == 6 && pid == self)
            return rss;
        line = end ? end + 1 : NULL;
    }
    return -1;
}

int main(void)
{
    long start = rss_kib();
    CHECK(start > 0, "no resident size in /dev/proc");
    CHECK(start < 4096, "%ld KiB resident at the start with 8 MiB of bss", start);
    bss[sizeof bss / 2] = 1;

    for (int i = 0; i < NBLOCKS; i++) {
        blocks[i] = malloc(BLOCK);
        if (!blocks[i]) {
            CHECK(0, "malloc %d", i);
            return 1;
        }
        memset(blocks[i], 0x5a, BLOCK);
    }
    char *top = malloc(BLOCK);
    CHECK(top != NULL, "malloc of the top block");
    long filled = rss_kib();
    CHECK(filled - start > 16 * 1024, "%ld KiB after writing 20 MiB, %ld at the start", filled, start);

    for (int i = 0; i < NBLOCKS; i++)
        free(blocks[i]);
    long inner = rss_kib();
    CHECK(filled - inner > 16 * 1024, "%ld KiB after free below a used top block, %ld before", inner, filled);

    free(top);
    long trimmed = rss_kib();
    CHECK(trimmed - start < 2048, "%ld KiB after freeing the top block, %ld at the start", trimmed, start);

    /* The pages given back return as zero pages. */
    for (int i = 0; i < NBLOCKS; i++) {
        blocks[i] = calloc(1, BLOCK);
        if (!blocks[i]) {
            CHECK(0, "calloc %d", i);
            return 1;
        }
        for (int j = 0; j < BLOCK; j++)
            if (blocks[i][j]) {
                CHECK(0, "calloc block %d byte %d is %d", i, j, blocks[i][j]);
                break;
            }
        memset(blocks[i], i & 0xff, BLOCK);
    }
    for (int i = 0; i < NBLOCKS; i++)
        if (blocks[i][BLOCK - 1] != (char)(i & 0xff)) {
            CHECK(0, "block %d lost its contents", i);
            break;
        }
    for (int i = 0; i < NBLOCKS; i++)
        free(blocks[i]);
    printf("memreleasetest: %ld KiB at the start, %ld filled, %ld after free inside the heap, %ld after the trim\n",
           start, filled, inner, trimmed);
    printf("memreleasetest: %d failures\n", failures);
    return failures ? 1 : 0;
}
