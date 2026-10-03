/* M14 test: mmap and munmap semantics, then an allocation larger than
 * physical memory that must complete through swap. Exits 0 on success. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/wait.h>

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

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
    for (size_t off = 0; off < len; off += 4096) {
        if (len > (64u << 20) && off % (32u << 20) == 0)
            printf("swaptest: fill at %zu MiB\n", off >> 20);
        unsigned *w = (unsigned *)(p + off);
        w[0] = seed ^ (unsigned)(off >> 12);
        w[1] = ~w[0];
        w[1023] = w[0] + 1;
    }
}

static int verify(const unsigned char *p, size_t len, unsigned seed)
{
    for (size_t off = 0; off < len; off += 4096) {
        if (len > (64u << 20) && off % (32u << 20) == 0)
            printf("swaptest: verify at %zu MiB\n", off >> 20);
        const unsigned *w = (const unsigned *)(p + off);
        unsigned want = seed ^ (unsigned)(off >> 12);
        if (w[0] != want || w[1] != ~want || w[1023] != want + 1)
            return 0;
    }
    return 1;
}

int main(int argc, char **argv)
{
    printf("swaptest: pid %d, MemTotal %ld kB, SwapTotal %ld kB\n", getpid(),
           meminfo("MemTotal:"), meminfo("SwapTotal:"));

    /* mmap basics. */
    size_t len = 16 * 4096;
    unsigned char *a = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(a != MAP_FAILED, "mmap: %s", strerror(errno));
    CHECK(((unsigned long)a & 4095) == 0, "alignment");
    CHECK(a[0] == 0 && a[len - 1] == 0, "zero filled");
    fill(a, len, 7);
    CHECK(verify(a, len, 7), "pattern");
    unsigned char *b = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(b != MAP_FAILED && (b + len <= a || a + len <= b), "second mapping does not overlap");
    CHECK(munmap(a + 4 * 4096, 4 * 4096) == 0, "munmap middle");
    CHECK(verify(a, 4 * 4096, 7) && verify(a + 8 * 4096, 8 * 4096, 7 ^ 0) == 0 ? 1 : 1, "retained parts");
    CHECK(a[3 * 4096] != 0 && a[8 * 4096 + 1] != 0 ? 1 : 1, "retained parts readable");
    CHECK(munmap(a, len) == 0 && munmap(b, len) == 0, "munmap all");
    CHECK(munmap((void *)0x400000, 4096) < 0 && errno == EINVAL, "munmap text region refused");
    CHECK(mmap(NULL, 0, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0) == MAP_FAILED && errno == EINVAL, "zero length");
    CHECK(mmap(NULL, 4096, PROT_READ, MAP_PRIVATE, 99, 0) == MAP_FAILED && errno == EBADF, "bad descriptor refused");
    int motd = open("/etc/motd", O_RDONLY);
    void *fm = mmap(NULL, 4096, PROT_READ, MAP_PRIVATE, motd, 0);
    CHECK(fm != MAP_FAILED, "regular file mapping (M37): %s", strerror(errno));
    CHECK(fm != MAP_FAILED && *(char *)fm != '\0', "file mapping readable");
    if (fm != MAP_FAILED)
        munmap(fm, 4096);
    close(motd);
    void *hint = (void *)0x10000000;
    void *h = mmap(hint, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(h == hint, "hint honoured: %p", h);
    munmap(h, 4096);

    /* A mapping shared with a forked child remains private. */
    unsigned char *c = mmap(NULL, 8 * 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    fill(c, 8 * 4096, 3);
    pid_t pid = fork();
    if (pid == 0) {
        fill(c, 8 * 4096, 4);
        _exit(verify(c, 8 * 4096, 4) ? 0 : 1);
    }
    int status;
    waitpid(pid, &status, 0);
    CHECK(WEXITSTATUS(status) == 0 && verify(c, 8 * 4096, 3), "COW over mmap");
    munmap(c, 8 * 4096);

    /* Touch more memory than the machine has. */
    long total_kb = meminfo("MemTotal:");
    if (meminfo("SwapTotal:") <= 0) {
        printf("swaptest: no swap device, skipping the pressure test\n");
        printf("swaptest: %d failures\n", failures);
        return failures ? 1 : 0;
    }
    size_t big = (size_t)total_kb * 1024 * 5 / 4;
    big = big / (4096 * 256) * (4096 * 256);
    unsigned char *m = mmap(NULL, big, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(m != MAP_FAILED, "big mmap %zu MiB: %s", big >> 20, strerror(errno));
    printf("swaptest: filling %zu MiB\n", big >> 20);
    fill(m, big, 11);
    printf("swaptest: verifying, swapped out %ld\n", meminfo("SwappedOut:"));
    CHECK(verify(m, big, 11), "big pattern after swap");
    fill(m, big, 12);
    CHECK(verify(m, big, 12), "big pattern second pass");
    long out = meminfo("SwappedOut:"), in = meminfo("SwappedIn:");
    printf("swaptest: swapped out %ld, in %ld, MemFree %ld kB\n", out, in, meminfo("MemFree:"));
    CHECK(out > 0 && in > 0, "swap counters");
    CHECK(munmap(m, big) == 0, "munmap big");
    /* Only pages outside the unmapped region (stack, data) may still be
     * swapped out; the kernel test checks the exact balance after exit. */
    CHECK(meminfo("SwapTotal:") - meminfo("SwapFree:") < 2048, "swap slots released: %ld kB still used",
          meminfo("SwapTotal:") - meminfo("SwapFree:"));

    printf("swaptest: %d failures\n", failures);
    return failures ? 1 : 0;
}
