/* M39 test: huge pages. MAP_HUGETLB and MADV_HUGEPAGE regions backed by
 * 2 MiB frames, fork with copy on write of a huge page, splitting by
 * partial munmap, mprotect and madvise, and the fallback to small pages.
 * Exits 0 on success. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/wait.h>

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)
#define PG 4096UL
#define HUGE (2UL << 20)

static long meminfo(const char *key)
{
    int fd = open("/dev/meminfo", O_RDONLY);
    if (fd < 0)
        return -1;
    char buf[512];
    ssize_t n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0)
        return -1;
    buf[n] = '\0';
    char *p = strstr(buf, key);
    return p ? atol(p + strlen(key)) : -1;
}

static void fill(unsigned char *p, size_t len, unsigned seed)
{
    for (size_t off = 0; off < len; off += PG) {
        unsigned *w = (unsigned *)(p + off);
        w[0] = seed ^ (unsigned)(off >> 12);
        w[1] = ~w[0];
        w[1023] = w[0] + 1;
    }
}

static int verify(const unsigned char *p, size_t len, unsigned seed)
{
    for (size_t off = 0; off < len; off += PG) {
        const unsigned *w = (const unsigned *)(p + off);
        unsigned want = seed ^ (unsigned)(off >> 12);
        if (w[0] != want || w[1] != ~want || w[1023] != want + 1) {
            printf("  mismatch at %p + %zx: %08x %08x %08x, want %08x\n", (const void *)p, off, w[0], w[1], w[1023], want);
            return 0;
        }
    }
    return 1;
}

/* verify for a window of a region filled from base. */
static int verify_at(const unsigned char *base, size_t off, size_t len, unsigned seed)
{
    for (size_t o = off; o < off + len; o += PG) {
        const unsigned *w = (const unsigned *)(base + o);
        unsigned want = seed ^ (unsigned)(o >> 12);
        if (w[0] != want || w[1] != ~want || w[1023] != want + 1) {
            printf("  mismatch at %p + %zx: %08x %08x %08x, want %08x\n", (const void *)base, o, w[0], w[1], w[1023], want);
            return 0;
        }
    }
    return 1;
}

static void touch_write(void *p) { volatile unsigned char *c = p; *c = 1; }

static int in_child(void (*fn)(void *), void *arg)
{
    pid_t pid = fork();
    if (pid == 0) {
        fn(arg);
        _exit(0);
    }
    int status = -1;
    waitpid(pid, &status, 0);
    return status;
}

static void test_hugetlb(void)
{
    long huge0 = meminfo("HugePages:");
    CHECK(mmap(NULL, HUGE + PG, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0) == MAP_FAILED &&
          errno == EINVAL, "MAP_HUGETLB length must be a multiple of 2 MiB");
    CHECK(mmap(NULL, HUGE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0) == MAP_FAILED &&
          errno == EINVAL, "MAP_HUGETLB must be private");
    unsigned char *a = mmap(NULL, 2 * HUGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);
    CHECK(a != MAP_FAILED, "MAP_HUGETLB: %s", strerror(errno));
    CHECK(((unsigned long)a & (HUGE - 1)) == 0, "2 MiB aligned: %p", (void *)a);
    CHECK(a[0] == 0 && a[2 * HUGE - 1] == 0, "zero filled");
    long huge1 = meminfo("HugePages:");
    CHECK(huge1 == huge0 + 2, "two huge pages mapped: %ld -> %ld", huge0, huge1);
    fill(a, 2 * HUGE, 1);
    CHECK(verify(a, 2 * HUGE, 1), "pattern across both huge pages");
    CHECK(meminfo("HugePages:") == huge0 + 2, "still two after writes");

    /* fork: the child inherits the huge pages copy on write. */
    pid_t pid = fork();
    if (pid == 0) {
        if (!verify(a, 2 * HUGE, 1))
            _exit(1);
        /* Both spaces map both frames: four entries. */
        if (meminfo("HugePages:") != huge0 + 4)
            _exit(2);
        a[HUGE + 100] = 0x42;           /* copies the second huge page into a new frame */
        if (meminfo("HugePages:") != huge0 + 4)
            _exit(3);
        if (a[HUGE + 100] != 0x42 || !verify(a, HUGE, 1))
            _exit(4);
        _exit(0);
    }
    int status;
    waitpid(pid, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0, "child copy on write of a huge page: status %x", status);
    CHECK(verify(a, 2 * HUGE, 1), "parent data intact after the child's write");
    CHECK(meminfo("HugePages:") == huge0 + 2, "child's copy released: %ld", meminfo("HugePages:"));
    a[5] = 7;                           /* parent takes back exclusive ownership */
    CHECK(a[5] == 7, "parent write after fork");
    fill(a, PG, 1);
    CHECK(verify(a, 2 * HUGE, 1), "data intact after the parent's copy on write");

    /* Partial munmap splits the first huge page into small pages. */
    long splits0 = meminfo("HugeSplits:");
    CHECK(munmap(a + 4 * PG, 2 * PG) == 0, "munmap a hole inside a huge page");
    CHECK(meminfo("HugeSplits:") == splits0 + 1, "one split");
    CHECK(meminfo("HugePages:") == huge0 + 1, "one huge page left");
    CHECK(verify(a, 4 * PG, 1) && verify_at(a, 6 * PG, HUGE - 6 * PG, 1), "data around the hole intact");
    CHECK(verify_at(a, HUGE, HUGE, 1), "second huge page intact");
    status = in_child(touch_write, a + 4 * PG);
    CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV, "the hole is unmapped: %x", status);

    /* mprotect of one small page inside the remaining huge page. */
    CHECK(mprotect(a + HUGE + 8 * PG, PG, PROT_READ) == 0, "mprotect inside a huge page: %s", strerror(errno));
    CHECK(meminfo("HugeSplits:") == splits0 + 2 && meminfo("HugePages:") == huge0, "split by mprotect");
    CHECK(verify_at(a, HUGE, HUGE, 1), "data intact after the split");
    status = in_child(touch_write, a + HUGE + 8 * PG);
    CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV, "protected page is read only: %x", status);
    a[HUGE + 9 * PG] = 3;
    CHECK(munmap(a, 2 * HUGE) == 0, "munmap the rest");
    CHECK(meminfo("HugePages:") == huge0, "no huge pages left");
}

static void test_madv_hugepage(void)
{
    long huge0 = meminfo("HugePages:");
    size_t len = 3 * HUGE;
    unsigned char *a = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(a != MAP_FAILED, "anonymous region");
    CHECK(madvise(a, len, MADV_HUGEPAGE) == 0, "MADV_HUGEPAGE");
    fill(a, len, 2);
    long got = meminfo("HugePages:") - huge0;
    /* Only 2 MiB blocks fully inside the region become huge pages. */
    long expect = (long)((((unsigned long)a + len) & ~(HUGE - 1)) - (((unsigned long)a + HUGE - 1) & ~(HUGE - 1))) / (long)HUGE;
    CHECK(got == expect, "aligned interior blocks are huge: %ld, expected %ld", got, expect);
    CHECK(verify(a, len, 2), "pattern intact");
    /* A huge page whose frame is unmapped as a whole by DONTNEED. */
    unsigned long first = ((unsigned long)a + HUGE - 1) & ~(HUGE - 1);
    long splits0 = meminfo("HugeSplits:");
    CHECK(madvise((void *)first, HUGE, MADV_DONTNEED) == 0, "DONTNEED a whole huge page");
    CHECK(meminfo("HugePages:") == huge0 + expect - 1 && meminfo("HugeSplits:") == splits0, "released without a split");
    CHECK(*(unsigned *)first == 0, "reads as zero");
    /* Refaulting gives a huge page again. */
    fill((unsigned char *)first, HUGE, 3);
    CHECK(meminfo("HugePages:") == huge0 + expect, "huge page refaulted");
    /* MADV_FREE splits huge pages so kswapd can reclaim small pages. */
    CHECK(madvise((void *)first, PG, MADV_FREE) == 0, "MADV_FREE on part of a huge page");
    CHECK(meminfo("HugeSplits:") == splits0 + 1, "split by MADV_FREE");
    CHECK(verify_at((unsigned char *)first, PG, HUGE - PG, 3), "data intact after the MADV_FREE split");
    CHECK(madvise(a, len, MADV_NOHUGEPAGE) == 0, "MADV_NOHUGEPAGE");
    CHECK(munmap(a, len) == 0, "munmap");
    CHECK(meminfo("HugePages:") == huge0, "all released: %ld", meminfo("HugePages:") - huge0);
}

static void test_fallback(void)
{
    /* Fragment physical memory: take nearly every free frame, then give
     * every other one back. Only single frames are free afterwards, so a
     * huge fault must fall back to small pages. */
    size_t chunk = 1u << 20;
    size_t max_chunks = 1024;
    unsigned char **chunks = malloc(max_chunks * sizeof *chunks);
    size_t got = 0;
    /* Stop with 1.5 MiB left: less than one 2 MiB block can then remain
     * unfragmented. The chunks come from the buddy allocator in nearly
     * physical order, so releasing every other virtual page leaves single
     * frames whose buddies are still in use. */
    while (got < max_chunks && meminfo("MemFree:") > 1536) {
        chunks[got] = mmap(NULL, chunk, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (chunks[got] == MAP_FAILED)
            break;
        for (size_t off = 0; off < chunk; off += PG)
            chunks[got][off] = 1;
        got++;
    }
    for (size_t i = 0; i < got; i++)
        for (size_t off = 0; off < chunk; off += 2 * PG)
            madvise(chunks[i] + off, PG, MADV_DONTNEED);
    printf("hugetest: %zu MiB touched, every other page released, MemFree %ld kB\n", got, meminfo("MemFree:"));
    long fb0 = meminfo("HugeFallbacks:");
    long huge0 = meminfo("HugePages:");
    unsigned char *h = mmap(NULL, HUGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);
    CHECK(h != MAP_FAILED, "MAP_HUGETLB under fragmentation");
    fill(h, HUGE, 9);
    CHECK(verify(h, HUGE, 9), "region usable without a huge frame");
    long fb1 = meminfo("HugeFallbacks:");
    long huge1 = meminfo("HugePages:");
    printf("hugetest: fallbacks %ld -> %ld, huge pages %ld -> %ld\n", fb0, fb1, huge0, huge1);
    CHECK(fb1 > fb0 && huge1 == huge0, "the fault fell back to small pages");
    munmap(h, HUGE);
    for (size_t i = 0; i < got; i++)
        munmap(chunks[i], chunk);
    free(chunks);
    /* With memory back, the same mapping is huge again. */
    h = mmap(NULL, HUGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);
    h[0] = 1;
    CHECK(meminfo("HugePages:") == huge0 + 1, "huge again once memory is free");
    munmap(h, HUGE);
}

int main(void)
{
    printf("hugetest: pid %d, MemTotal %ld kB\n", getpid(), meminfo("MemTotal:"));
    test_hugetlb();
    test_madv_hugepage();
    test_fallback();
    printf("hugetest: %d failures\n", failures);
    return failures ? 1 : 0;
}
