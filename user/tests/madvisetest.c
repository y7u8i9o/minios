/* M38 test: madvise. Every advice on anonymous and file regions,
 * MADV_FREE under memory pressure, MADV_DONTNEED on swapped pages and
 * MADV_DONTFORK. Exits 0 on success. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/stat.h>

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)
#define PG 4096
#define PATH "/madvtest/data"

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

/* 1 when the page holds the pattern, 0 when it is all zero, -1 otherwise. */
static int page_state(const unsigned char *p, size_t off, unsigned seed)
{
    const unsigned *w = (const unsigned *)(p + off);
    unsigned want = seed ^ (unsigned)(off >> 12);
    if (w[0] == want && w[1] == ~want && w[1023] == want + 1)
        return 1;
    if (w[0] == 0 && w[1] == 0 && w[1023] == 0)
        return 0;
    return -1;
}

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

static void touch_read(void *p) { volatile unsigned char *c = p; (void)*c; }

static void test_basic(void)
{
    size_t len = 16 * PG;
    unsigned char *a = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(a != MAP_FAILED, "mmap");
    fill(a, len, 5);
    CHECK(madvise(a, len, MADV_NORMAL) == 0, "MADV_NORMAL: %s", strerror(errno));
    CHECK(madvise(a, len, MADV_RANDOM) == 0, "MADV_RANDOM");
    CHECK(madvise(a + PG, 2 * PG, MADV_SEQUENTIAL) == 0, "MADV_SEQUENTIAL on part of the region");
    CHECK(madvise(a, len, MADV_WILLNEED) == 0, "MADV_WILLNEED");
    CHECK(madvise(a, 0, MADV_NORMAL) == 0, "zero length");
    CHECK(madvise(a + 1, PG, MADV_NORMAL) < 0 && errno == EINVAL, "unaligned address");
    CHECK(madvise(a, len, 99) < 0 && errno == EINVAL, "unknown advice");
    CHECK(madvise(a, len + PG, MADV_NORMAL) < 0 && errno == ENOMEM, "range past the mapping: %s", strerror(errno));
    CHECK(madvise((void *)0x100000000000UL, PG, MADV_NORMAL) < 0 && errno == ENOMEM, "unmapped range");
    int ok = 1;
    for (size_t off = 0; off < len; off += PG)
        if (page_state(a, off, 5) != 1)
            ok = 0;
    CHECK(ok, "data intact after advice");

    /* DONTNEED drops the middle pages; they read as zero afterwards. */
    CHECK(madvise(a + 4 * PG, 4 * PG, MADV_DONTNEED) == 0, "MADV_DONTNEED");
    ok = 1;
    for (size_t off = 0; off < len; off += PG) {
        int want = off >= 4 * PG && off < 8 * PG ? 0 : 1;
        if (page_state(a, off, 5) != want)
            ok = 0;
    }
    CHECK(ok, "DONTNEED zeroed exactly the middle pages");
    fill(a + 4 * PG, 4 * PG, 6);
    CHECK(page_state(a + 4 * PG, 0, 6) == 1, "pages usable again after DONTNEED");
    /* Regions were split by the advice; unmapping in pieces still works. */
    CHECK(munmap(a, len) == 0, "munmap after splits");

    /* WILLNEED on a fresh region makes it resident: reads still give zero. */
    unsigned char *b = mmap(NULL, 8 * PG, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    long before = meminfo("MemFree:");
    CHECK(madvise(b, 8 * PG, MADV_WILLNEED) == 0, "WILLNEED fresh");
    long after = meminfo("MemFree:");
    CHECK(before - after >= 8 * 4, "WILLNEED made %ld kB resident", before - after);
    ok = 1;
    for (size_t off = 0; off < 8 * PG; off += PG)
        if (page_state(b, off, 0) != 0)
            ok = 0;
    CHECK(ok, "prefaulted pages are zero");
    CHECK(mprotect(b, PG, PROT_NONE) == 0 && madvise(b, PG, MADV_WILLNEED) == 0, "WILLNEED on PROT_NONE is accepted");
    munmap(b, 8 * PG);
}

static void test_file(void)
{
    mkdir("/madvtest", 0755);
    int fd = open(PATH, O_CREAT | O_TRUNC | O_RDWR, 0644);
    CHECK(fd >= 0, "create file");
    unsigned char buf[4 * PG];
    for (size_t i = 0; i < sizeof buf; i++)
        buf[i] = (unsigned char)(i * 3);
    CHECK(write(fd, buf, sizeof buf) == (ssize_t)sizeof buf, "write file");
    unsigned char *s = mmap(NULL, 4 * PG, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    CHECK(s != MAP_FAILED, "shared file mapping");
    s[PG + 1] = 0xab;
    CHECK(madvise(s, 4 * PG, MADV_DONTNEED) == 0, "DONTNEED on a shared file mapping");
    CHECK(s[PG + 1] == 0xab && s[3] == (unsigned char)9, "shared page kept its data in the cache: %02x", s[PG + 1]);
    CHECK(madvise(s, 4 * PG, MADV_WILLNEED) == 0, "WILLNEED on a file mapping");
    CHECK(madvise(s, PG, MADV_FREE) < 0 && errno == EINVAL, "MADV_FREE on a shared mapping refused");
    CHECK(madvise(s, PG, MADV_HUGEPAGE) < 0 && errno == EINVAL, "MADV_HUGEPAGE on a file mapping refused");
    CHECK(munmap(s, 4 * PG) == 0, "munmap shared");
    lseek(fd, PG + 1, SEEK_SET);
    unsigned char c = 0;
    CHECK(read(fd, &c, 1) == 1 && c == 0xab, "dirty page dropped by DONTNEED reached the file: %02x", c);

    unsigned char *p = mmap(NULL, 4 * PG, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
    CHECK(p != MAP_FAILED, "private file mapping");
    p[5] = 0xcd;
    CHECK(madvise(p, PG, MADV_DONTNEED) == 0, "DONTNEED on a private file page");
    CHECK(p[5] == (unsigned char)15, "private copy dropped, file content back: %02x", p[5]);
    CHECK(madvise(p, PG, MADV_FREE) < 0 && errno == EINVAL, "MADV_FREE on a private file mapping refused");
    munmap(p, 4 * PG);
    close(fd);
    unlink(PATH);
    rmdir("/madvtest");
}

static void child_touch(void *arg)
{
    touch_read(arg);
}

static void test_dontfork(void)
{
    unsigned char *a = mmap(NULL, 2 * PG, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    fill(a, 2 * PG, 9);
    CHECK(madvise(a, 2 * PG, MADV_DONTFORK) == 0, "MADV_DONTFORK");
    int status = in_child(child_touch, a);
    CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV, "child has no DONTFORK region: status %x", status);
    CHECK(page_state(a, 0, 9) == 1 && page_state(a, PG, 9) == 1, "parent keeps the region");
    CHECK(madvise(a, 2 * PG, MADV_DOFORK) == 0, "MADV_DOFORK");
    pid_t pid = fork();
    if (pid == 0)
        _exit(page_state(a, PG, 9) == 1 ? 0 : 1);
    waitpid(pid, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0, "child sees the region again: %x", status);
    CHECK(madvise(a, 2 * PG, MADV_HUGEPAGE) == 0 && madvise(a, 2 * PG, MADV_NOHUGEPAGE) == 0, "huge page advice accepted");
    munmap(a, 2 * PG);
}

static void test_free_under_pressure(void)
{
    if (meminfo("SwapTotal:") <= 0) {
        printf("madvisetest: no swap device, skipping the pressure test\n");
        return;
    }
    size_t len = 64 * PG;
    unsigned char *a = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(a != MAP_FAILED, "region for MADV_FREE");
    fill(a, len, 21);
    long lazy0 = meminfo("LazyFreed:");
    CHECK(madvise(a, len, MADV_FREE) == 0, "MADV_FREE");
    /* Half the pages are written again; they must survive. */
    fill(a, len / 2, 22);
    /* A page read after MADV_FREE stays a candidate. */
    CHECK(page_state(a, len / 2, 21) == 1, "page still holds its data before pressure");

    /* Pressure: touch more memory than is free so kswapd reclaims. */
    long free_kb = meminfo("MemFree:");
    size_t big = (size_t)free_kb * 1024 + (16u << 20);
    big = big / (PG * 256) * (PG * 256);
    unsigned char *m = mmap(NULL, big, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(m != MAP_FAILED, "big mapping %zu MiB", big >> 20);
    printf("madvisetest: filling %zu MiB for pressure\n", big >> 20);
    fill(m, big, 31);
    long lazy1 = meminfo("LazyFreed:");
    printf("madvisetest: LazyFreed %ld -> %ld, swapped out %ld\n", lazy0, lazy1, meminfo("SwappedOut:"));
    CHECK(lazy1 > lazy0, "kswapd discarded lazily freed pages");
    int kept = 0, zeroed = 0, bad = 0;
    for (size_t off = 0; off < len / 2; off += PG)
        if (page_state(a, off, 22) != 1)
            bad++;
    CHECK(bad == 0, "%d rewritten pages lost their data", bad);
    for (size_t off = len / 2; off < len; off += PG) {
        int st = page_state(a, off, 21);
        if (st == 1) kept++;
        else if (st == 0) zeroed++;
        else bad++;
    }
    printf("madvisetest: freed pages: %d kept, %d zeroed, %d bad\n", kept, zeroed, bad);
    CHECK(bad == 0, "freed pages hold neither their data nor zero");
    CHECK(zeroed > 0, "no lazily freed page was discarded");
    CHECK(page_state(m, 0, 31) == 1 && page_state(m, big - PG, 31) == 1, "pressure data intact");

    /* DONTNEED over a range that is partly swapped out frees the slots. */
    long swapfree0 = meminfo("SwapFree:");
    CHECK(madvise(m, big, MADV_DONTNEED) == 0, "DONTNEED over swapped pages");
    long swapfree1 = meminfo("SwapFree:");
    CHECK(swapfree1 >= swapfree0, "swap slots released: %ld -> %ld", swapfree0, swapfree1);
    CHECK(page_state(m, 0, 31) == 0 && page_state(m, big / 2, 31) == 0, "swapped pages read as zero after DONTNEED");
    /* MADV_FREE on swapped pages drops them right away. */
    fill(m, big, 32);
    swapfree0 = meminfo("SwapFree:");
    CHECK(madvise(m, big, MADV_FREE) == 0, "MADV_FREE over swapped pages");
    CHECK(meminfo("SwapFree:") > swapfree0, "MADV_FREE released swap slots");
    munmap(m, big);
    munmap(a, len);
}

int main(void)
{
    printf("madvisetest: pid %d, MemTotal %ld kB, SwapTotal %ld kB\n", getpid(),
           meminfo("MemTotal:"), meminfo("SwapTotal:"));
    test_basic();
    test_file();
    test_dontfork();
    test_free_under_pressure();
    printf("madvisetest: %d failures\n", failures);
    return failures ? 1 : 0;
}
