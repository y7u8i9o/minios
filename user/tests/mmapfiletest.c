/* M37 test: file backed mappings. Private and shared mappings of a file on
 * the root disk, offsets, aliasing inside one process and across fork,
 * coherence with read and write, writeback, faults beyond the end of the
 * file, mprotect and MAP_FIXED. Exits 0 on success. */
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
#define PATH "/mmaptest/data"
#define NPAGES 6                        /* the file is 5.5 pages long */
#define FILE_SIZE (NPAGES * PG - PG / 2)

static unsigned char pattern(size_t off, unsigned seed)
{
    return (unsigned char)((off * 7 + seed) ^ (off >> 8));
}

static void make_file(unsigned seed)
{
    int fd = open(PATH, O_CREAT | O_TRUNC | O_RDWR, 0644);
    CHECK(fd >= 0, "create %s: %s", PATH, strerror(errno));
    unsigned char *buf = malloc(FILE_SIZE);
    for (size_t i = 0; i < FILE_SIZE; i++)
        buf[i] = pattern(i, seed);
    CHECK(write(fd, buf, FILE_SIZE) == FILE_SIZE, "write pattern");
    free(buf);
    close(fd);
}

static int file_matches(const unsigned char *want, size_t off, size_t n)
{
    int fd = open(PATH, O_RDONLY);
    if (fd < 0)
        return 0;
    unsigned char *buf = malloc(n);
    lseek(fd, (long)off, SEEK_SET);
    size_t got = 0;
    while (got < n) {
        ssize_t r = read(fd, buf + got, n - got);
        if (r <= 0)
            break;
        got += (size_t)r;
    }
    close(fd);
    int ok = got == n && memcmp(buf, want, n) == 0;
    if (!ok)
        for (size_t i = 0; i < got && i < n; i++)
            if (buf[i] != want[i]) {
                printf("  mismatch at %zu: %02x, want %02x (got %zu bytes)\n", off + i, buf[i], want[i], got);
                break;
            }
    free(buf);
    return ok;
}

/* Run fn in a child and return the wait status. */
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
static void touch_write(void *p) { volatile unsigned char *c = p; *c = 1; }

static void test_private(void)
{
    make_file(1);
    int fd = open(PATH, O_RDONLY);
    CHECK(fd >= 0, "open for private mapping");
    unsigned char *p = mmap(NULL, NPAGES * PG, PROT_READ, MAP_PRIVATE, fd, 0);
    CHECK(p != MAP_FAILED, "private mapping: %s", strerror(errno));
    int ok = 1;
    for (size_t i = 0; i < FILE_SIZE; i++)
        if (p[i] != pattern(i, 1)) { ok = 0; break; }
    CHECK(ok, "private mapping content");
    ok = 1;
    for (size_t i = FILE_SIZE; i < NPAGES * PG; i++)
        if (p[i] != 0) { ok = 0; break; }
    CHECK(ok, "tail of the last page is zero filled");
    /* A page entirely beyond the end of the file is not readable. */
    unsigned char *q = mmap(NULL, (NPAGES + 2) * PG, PROT_READ, MAP_PRIVATE, fd, 0);
    CHECK(q != MAP_FAILED, "mapping longer than the file");
    int status = in_child(touch_read, q + NPAGES * PG);
    CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV, "read beyond the end faults: status %x", status);
    CHECK(munmap(q, (NPAGES + 2) * PG) == 0, "munmap long mapping");
    /* Writing to a read only mapping faults. */
    status = in_child(touch_write, p);
    CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV, "write to PROT_READ faults: status %x", status);
    CHECK(munmap(p, NPAGES * PG) == 0, "munmap private");

    /* A writable private mapping never changes the file. */
    p = mmap(NULL, NPAGES * PG, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
    CHECK(p != MAP_FAILED, "private writable mapping of a read only descriptor: %s", strerror(errno));
    CHECK(p[10] == pattern(10, 1), "content before the private write");
    p[10] = 0xaa;
    memset(p + 2 * PG, 0xbb, PG);
    CHECK(p[10] == 0xaa && p[2 * PG] == 0xbb, "private writes visible to the writer");
    unsigned char want[PG];
    for (size_t i = 0; i < PG; i++)
        want[i] = pattern(i, 1);
    CHECK(file_matches(want, 0, PG), "file unchanged by private writes");
    CHECK(msync(p, NPAGES * PG, MS_SYNC) == 0, "msync on a private mapping");
    CHECK(file_matches(want, 0, PG), "file unchanged by msync of a private mapping");
    /* Another private mapping of the same page still sees the file. */
    unsigned char *r = mmap(NULL, PG, PROT_READ, MAP_PRIVATE, fd, 0);
    CHECK(r != MAP_FAILED && r[10] == pattern(10, 1), "second private mapping sees the file, not the copy");
    munmap(r, PG);
    munmap(p, NPAGES * PG);
    close(fd);
    CHECK(file_matches(want, 0, PG), "file unchanged after unmapping private copies");
}

struct shared_arg { unsigned char *p; };

static void child_writes_shared(void *arg)
{
    struct shared_arg *a = arg;
    a->p[100] = 0x5a;
    memset(a->p + 3 * PG, 0x3c, 64);
}

static void test_shared(void)
{
    make_file(2);
    int fd = open(PATH, O_RDWR);
    CHECK(fd >= 0, "open for shared mapping");
    CHECK(mmap(NULL, PG, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 1) == MAP_FAILED && errno == EINVAL,
          "unaligned offset refused");
    CHECK(mmap(NULL, PG, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_PRIVATE, fd, 0) == MAP_FAILED && errno == EINVAL,
          "shared and private together refused");
    CHECK(mmap(NULL, PG, PROT_READ | PROT_WRITE, 0, fd, 0) == MAP_FAILED && errno == EINVAL,
          "neither shared nor private refused");
    int ro = open(PATH, O_RDONLY);
    CHECK(mmap(NULL, PG, PROT_READ | PROT_WRITE, MAP_SHARED, ro, 0) == MAP_FAILED && errno == EACCES,
          "shared writable mapping of a read only descriptor refused");
    unsigned char *rop = mmap(NULL, PG, PROT_READ, MAP_SHARED, ro, 0);
    CHECK(rop != MAP_FAILED, "shared read only mapping of a read only descriptor");
    CHECK(mprotect(rop, PG, PROT_READ | PROT_WRITE) < 0 && errno == EACCES,
          "mprotect cannot make it writable");
    close(ro);

    unsigned char *p = mmap(NULL, NPAGES * PG, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    CHECK(p != MAP_FAILED, "shared mapping: %s", strerror(errno));
    CHECK(p[5] == pattern(5, 2), "shared mapping content");
    /* Two mappings of the same file alias the same memory. */
    unsigned char *alias = mmap(NULL, 2 * PG, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 2 * PG);
    CHECK(alias != MAP_FAILED, "aliasing mapping with an offset");
    CHECK(alias[0] == pattern(2 * PG, 2) && alias[PG + 17] == pattern(3 * PG + 17, 2), "offset mapping content");
    alias[5] = 0x77;
    CHECK(p[2 * PG + 5] == 0x77, "write through one mapping visible through the other");
    p[3 * PG + 9] = 0x66;
    CHECK(alias[PG + 9] == 0x66, "and the other way round");
    CHECK(rop[5] == pattern(5, 2), "read only shared mapping sees the file");
    p[5] = 0x11;
    CHECK(rop[5] == 0x11, "read only shared mapping sees a shared write");
    munmap(rop, PG);

    /* read() sees the mapped writes before any writeback. */
    unsigned char buf[16];
    lseek(fd, 2 * PG, SEEK_SET);
    CHECK(read(fd, buf, 16) == 16 && buf[5] == 0x77, "read sees the shared write");
    /* write() reaches the mapping. */
    lseek(fd, PG + 100, SEEK_SET);
    CHECK(write(fd, "hello", 5) == 5, "write into a mapped file");
    CHECK(memcmp(p + PG + 100, "hello", 5) == 0, "mapping sees the write");

    /* A child writes through the shared mapping, the parent sees it. */
    struct shared_arg a = { p };
    int status = in_child(child_writes_shared, &a);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0, "child status %x", status);
    CHECK(p[100] == 0x5a && p[3 * PG] == 0x3c && p[3 * PG + 63] == 0x3c && alias[PG + 63] == 0x3c,
          "child's writes visible through the parent's mappings");

    /* Writeback: msync puts the data on disk; without the mapping the read
     * path has nothing to overlay, so drop every mapping first. */
    CHECK(msync(p, NPAGES * PG, MS_SYNC) == 0, "msync: %s", strerror(errno));
    CHECK(msync(p + PG, PG, MS_ASYNC) == 0, "msync async");
    CHECK(msync(p, PG, MS_SYNC | MS_ASYNC) < 0 && errno == EINVAL, "msync sync and async together");
    CHECK(msync((void *)0x10, PG, MS_SYNC) < 0 && errno == EINVAL, "msync unaligned");
    unsigned char want[NPAGES * PG];
    for (size_t i = 0; i < FILE_SIZE; i++)
        want[i] = pattern(i, 2);
    want[5] = 0x11;
    want[100] = 0x5a;
    memcpy(want + PG + 100, "hello", 5);
    want[2 * PG + 5] = 0x77;
    memset(want + 3 * PG, 0x3c, 64);
    CHECK(munmap(alias, 2 * PG) == 0 && munmap(p, NPAGES * PG) == 0, "munmap shared");
    close(fd);
    CHECK(file_matches(want, 0, FILE_SIZE), "file holds the shared writes after msync and munmap");

    /* Writes without msync are written back by munmap. */
    fd = open(PATH, O_RDWR);
    p = mmap(NULL, NPAGES * PG, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    CHECK(p != MAP_FAILED, "remap shared");
    memset(p + 4 * PG, 0xee, PG);
    p[FILE_SIZE - 1] = 0xdd;
    p[FILE_SIZE] = 0xcc;            /* beyond the size: inside the last page, never written */
    memset(want + 4 * PG, 0xee, PG);
    want[FILE_SIZE - 1] = 0xdd;
    CHECK(munmap(p, NPAGES * PG) == 0, "munmap dirty shared");
    close(fd);
    CHECK(file_matches(want, 0, FILE_SIZE), "munmap wrote the dirty pages back");
    int fd2 = open(PATH, O_RDONLY);
    long size = lseek(fd2, 0, SEEK_END);
    close(fd2);
    CHECK(size == FILE_SIZE, "size unchanged by writes to the tail of the last page: %ld", size);
}

static void child_sees_private_copy(void *arg)
{
    unsigned char *p = arg;
    /* The parent's private copy (0xa0) is inherited copy on write; the
     * child's own write must not reach the parent. */
    if (p[0] != 0xa0)
        _exit(1);
    p[0] = 0xa1;
    if (p[0] != 0xa1)
        _exit(2);
    _exit(0);
}

static void test_fork_private(void)
{
    make_file(3);
    int fd = open(PATH, O_RDONLY);
    unsigned char *p = mmap(NULL, 2 * PG, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
    CHECK(p != MAP_FAILED, "private mapping for fork");
    p[0] = 0xa0;
    int status = in_child(child_sees_private_copy, p);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0, "child private view status %x", status);
    CHECK(p[0] == 0xa0, "child's write did not reach the parent's copy");
    /* An untouched page is inherited as the cached file page and each
     * side can still copy it privately. */
    CHECK(p[PG + 3] == pattern(PG + 3, 3), "second page content");
    p[PG + 3] = 0x42;
    unsigned char want = pattern(PG + 3, 3);
    CHECK(file_matches(&want, PG + 3, 1), "file byte unchanged");
    munmap(p, 2 * PG);
    close(fd);
}

static void test_mprotect(void)
{
    unsigned char *p = mmap(NULL, 4 * PG, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(p != MAP_FAILED, "anonymous mapping for mprotect");
    memset(p, 0x31, 4 * PG);
    CHECK(mprotect(p + PG, PG, PROT_READ) == 0, "mprotect middle page read only: %s", strerror(errno));
    CHECK(p[PG] == 0x31 && p[2 * PG] == 0x31, "data kept across mprotect");
    int status = in_child(touch_write, p + PG);
    CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV, "write to read only page faults: %x", status);
    p[2 * PG] = 0x32;
    CHECK(mprotect(p + PG, PG, PROT_READ | PROT_WRITE) == 0, "writable again");
    p[PG] = 0x33;
    CHECK(p[PG] == 0x33 && p[2 * PG] == 0x32, "writes after restoring protection");
    CHECK(mprotect(p + 2 * PG, PG, PROT_NONE) == 0, "PROT_NONE");
    status = in_child(touch_read, p + 2 * PG);
    CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV, "read of PROT_NONE page faults: %x", status);
    CHECK(mprotect(p + 2 * PG, PG, PROT_READ) == 0, "readable again");
    CHECK(p[2 * PG] == 0x32, "data survived PROT_NONE: %02x", p[2 * PG]);
    /* A PROT_NONE page whose frame is shared with a child after fork. */
    CHECK(mprotect(p + 3 * PG, PG, PROT_NONE) == 0, "PROT_NONE before fork");
    pid_t pid = fork();
    if (pid == 0) {
        if (mprotect(p + 3 * PG, PG, PROT_READ | PROT_WRITE) < 0)
            _exit(1);
        if (p[3 * PG] != 0x31)
            _exit(2);
        p[3 * PG] = 0x34;
        _exit(0);
    }
    waitpid(pid, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0, "child restored PROT_NONE page: %x", status);
    CHECK(mprotect(p + 3 * PG, PG, PROT_READ) == 0, "parent restores");
    CHECK(p[3 * PG] == 0x31, "child's copy on write did not reach the parent: %02x", p[3 * PG]);
    /* Errors. */
    CHECK(mprotect(p + 1, PG, PROT_READ) < 0 && errno == EINVAL, "unaligned mprotect");
    CHECK(mprotect(p, 4 * PG + PG, PROT_READ) < 0 && errno == ENOMEM, "mprotect past the mapping: %s", strerror(errno));
    CHECK(mprotect(p, PG, 0x100) < 0 && errno == EINVAL, "bad protection bits");
    /* The split regions can be unmapped piecewise. */
    CHECK(munmap(p + PG, PG) == 0, "unmap one split piece");
    CHECK(munmap(p, 4 * PG) == 0, "unmap the rest");
    /* Text is not a mmap region but can be reprotected. */
    extern char _start[];
    (void)_start;
}

static void test_fixed(void)
{
    unsigned char *a = mmap(NULL, 4 * PG, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(a != MAP_FAILED, "anonymous mapping for MAP_FIXED");
    memset(a, 0x51, 4 * PG);
    /* Replace the middle two pages with a file mapping. */
    make_file(4);
    int fd = open(PATH, O_RDONLY);
    unsigned char *f = mmap(a + PG, 2 * PG, PROT_READ, MAP_PRIVATE | MAP_FIXED, fd, PG);
    CHECK(f == a + PG, "MAP_FIXED placed the mapping: %p vs %p (%s)", (void *)f, (void *)(a + PG), strerror(errno));
    CHECK(a[0] == 0x51 && a[3 * PG] == 0x51, "pages around the fixed mapping kept");
    CHECK(f[0] == pattern(PG, 4) && f[PG] == pattern(2 * PG, 4), "fixed file mapping content");
    /* A fixed anonymous mapping over the whole range replaces everything. */
    unsigned char *z = mmap(a, 4 * PG, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    CHECK(z == a, "MAP_FIXED over mixed regions");
    CHECK(z[0] == 0 && z[PG] == 0 && z[3 * PG] == 0, "replaced pages are zero");
    CHECK(mmap((void *)0x400000, PG, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == MAP_FAILED &&
          errno == EINVAL, "MAP_FIXED over the program text refused");
    CHECK(mmap(a + 1, PG, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == MAP_FAILED &&
          errno == EINVAL, "unaligned MAP_FIXED refused");
    munmap(a, 4 * PG);
    close(fd);
}

static void test_anon_shared(void)
{
    unsigned char *p = mmap(NULL, 2 * PG, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    CHECK(p != MAP_FAILED, "anonymous shared mapping: %s", strerror(errno));
    CHECK(p[0] == 0 && p[2 * PG - 1] == 0, "anonymous shared mapping is zero");
    p[7] = 1;
    pid_t pid = fork();
    if (pid == 0) {
        if (p[7] != 1)
            _exit(1);
        p[7] = 2;
        p[PG] = 3;
        _exit(0);
    }
    int status;
    waitpid(pid, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0, "anonymous shared child status %x", status);
    CHECK(p[7] == 2 && p[PG] == 3, "anonymous shared writes of the child visible: %d %d", p[7], p[PG]);
    munmap(p, 2 * PG);
}

static void test_truncate_and_exec_cleanup(void)
{
    make_file(5);
    int fd = open(PATH, O_RDWR);
    unsigned char *p = mmap(NULL, NPAGES * PG, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    CHECK(p != MAP_FAILED, "shared mapping before truncation");
    CHECK(p[PG] == pattern(PG, 5), "content");
    close(fd);
    /* Truncating the file through O_TRUNC drops its cached pages. */
    fd = open(PATH, O_WRONLY | O_TRUNC);
    CHECK(fd >= 0, "reopen with O_TRUNC");
    CHECK(write(fd, "new", 3) == 3, "write after truncation");
    close(fd);
    CHECK(memcmp(p, "new", 3) == 0, "mapping sees the rewritten start: %.3s", (char *)p);
    p[1] = 'o';
    munmap(p, NPAGES * PG);
    CHECK(file_matches((const unsigned char *)"now", 0, 3), "writeback of a page after truncation");
    /* A mapping alive at exit is written back by the kernel. */
    make_file(6);
    pid_t pid = fork();
    if (pid == 0) {
        int f = open(PATH, O_RDWR);
        unsigned char *q = mmap(NULL, PG, PROT_READ | PROT_WRITE, MAP_SHARED, f, PG);
        if (q == MAP_FAILED)
            _exit(1);
        memset(q, 0x99, PG);
        _exit(0);                   /* no munmap, no msync */
    }
    int status;
    waitpid(pid, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0, "exiting child status %x", status);
    unsigned char want[PG];
    memset(want, 0x99, PG);
    CHECK(file_matches(want, PG, PG), "exit wrote the dirty page back");
}

int main(int argc, char **argv)
{
    printf("mmapfiletest: pid %d\n", getpid());
    if (mkdir("/mmaptest", 0755) < 0 && errno != EEXIST)
        CHECK(0, "mkdir /mmaptest: %s", strerror(errno));
    test_private();
    test_shared();
    test_fork_private();
    test_mprotect();
    test_fixed();
    test_anon_shared();
    test_truncate_and_exec_cleanup();
    unlink(PATH);
    rmdir("/mmaptest");
    printf("mmapfiletest: %d failures\n", failures);
    return failures ? 1 : 0;
}
