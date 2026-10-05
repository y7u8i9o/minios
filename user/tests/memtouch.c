/* memtouch MIB: maps MIB MiB of anonymous memory, writes a pattern into
 * every page and reads it back. Exits 0 when every page has its pattern.
 * The boot case balloon_oom runs it to allocate more memory than is free
 * (V4 of docs/plan/release-0.6.0.md). */
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>

int main(int argc, char **argv)
{
    if (argc != 2 || atol(argv[1]) <= 0) {
        fprintf(stderr, "usage: memtouch MIB\n");
        return 2;
    }
    size_t len = (size_t)atol(argv[1]) << 20;
    unsigned char *p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        perror("memtouch: mmap");
        return 1;
    }
    for (size_t off = 0; off < len; off += 4096)
        *(size_t *)(p + off) = off ^ 0x5a5a5a5a;
    for (size_t off = 0; off < len; off += 4096)
        if (*(size_t *)(p + off) != (off ^ 0x5a5a5a5a)) {
            printf("memtouch: wrong pattern at %zu\n", off);
            return 1;
        }
    printf("memtouch: %s MiB written and read\n", argv[1]);
    return 0;
}
