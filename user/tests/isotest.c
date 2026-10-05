/* The ISO 9660 file system (R5 of docs/plan/release-0.5.0.md), run by
 * /etc/tests/iso9660.sh in the cases iso9660 and iso9660_plain. The
 * program walks the tree below its argument depth first, with the names
 * of each directory in strcmp order, and prints one line per entry:
 *
 *   ISO:path|type|permissions|uid|gid|size|mtime|extra
 *
 * type is d, f or l, permissions are octal with the set id bits, size is
 * "-" for a directory, and extra is the FNV-1a hash of a file's contents
 * or the target of a link. The post script of the case compares the lines
 * with the manifest that the image script wrote from the host tree. The
 * program also checks that mmap of the largest file returns the bytes that
 * read returns, that a read at an offset in its middle does as well, and
 * that writing, creating and removing fail with EROFS. Exits 0 when these
 * checks pass. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/mman.h>

static int failures, entries;
static char largest[1024];
static off_t largest_size = -1;

#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

static uint64_t fnv_file(const char *path)
{
    uint64_t h = 0xcbf29ce484222325ull;
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        CHECK(0, "open %s: %s", path, strerror(errno));
        return 0;
    }
    static unsigned char buf[65536];
    ssize_t n;
    while ((n = read(fd, buf, sizeof buf)) > 0)
        for (ssize_t i = 0; i < n; i++) {
            h ^= buf[i];
            h *= 0x100000001b3ull;
        }
    CHECK(n == 0, "read %s: %s", path, strerror(errno));
    close(fd);
    return h;
}

static int compare(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static void walk(const char *root, const char *rel)
{
    char dirpath[1024];
    snprintf(dirpath, sizeof dirpath, "%s%s%s", root, rel[0] ? "/" : "", rel);
    DIR *d = opendir(dirpath);
    if (!d) {
        CHECK(0, "opendir %s: %s", dirpath, strerror(errno));
        return;
    }
    char **names = NULL;
    size_t n = 0, cap = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        if (n == cap) {
            cap = cap ? 2 * cap : 64;
            names = realloc(names, cap * sizeof *names);
        }
        names[n++] = strdup(e->d_name);
    }
    closedir(d);
    qsort(names, n, sizeof *names, compare);
    for (size_t i = 0; i < n; i++) {
        char path[1024], relpath[1024];
        snprintf(relpath, sizeof relpath, "%s%s%s", rel, rel[0] ? "/" : "", names[i]);
        snprintf(path, sizeof path, "%s/%s", root, relpath);
        struct stat st;
        if (lstat(path, &st) < 0) {
            CHECK(0, "lstat %s: %s", path, strerror(errno));
            continue;
        }
        entries++;
        unsigned perm = st.st_mode & 07777;
        if (S_ISDIR(st.st_mode)) {
            printf("ISO:%s|d|%04o|%u|%u|-|%lld|\n", relpath, perm, (unsigned)st.st_uid, (unsigned)st.st_gid,
                   (long long)st.st_mtime);
            walk(root, relpath);
        } else if (S_ISLNK(st.st_mode)) {
            char target[512];
            ssize_t len = readlink(path, target, sizeof target - 1);
            target[len > 0 ? len : 0] = '\0';
            printf("ISO:%s|l|%04o|%u|%u|%lld|%lld|%s\n", relpath, perm, (unsigned)st.st_uid, (unsigned)st.st_gid,
                   (long long)st.st_size, (long long)st.st_mtime, target);
        } else {
            printf("ISO:%s|f|%04o|%u|%u|%lld|%lld|%016llx\n", relpath, perm, (unsigned)st.st_uid,
                   (unsigned)st.st_gid, (long long)st.st_size, (long long)st.st_mtime,
                   (unsigned long long)fnv_file(path));
            if (st.st_size > largest_size) {
                largest_size = st.st_size;
                strcpy(largest, path);
            }
        }
        free(names[i]);
    }
    free(names);
}

/* mmap of the largest file against read, and a read in its middle. */
static void check_map(void)
{
    if (largest_size <= 0)
        return;
    int fd = open(largest, O_RDONLY);
    CHECK(fd >= 0, "open %s", largest);
    if (fd < 0)
        return;
    char *buf = malloc((size_t)largest_size);
    ssize_t got = 0, n;
    while (got < largest_size && (n = read(fd, buf + got, (size_t)(largest_size - got))) > 0)
        got += n;
    CHECK(got == largest_size, "read %s: %zd of %lld bytes", largest, got, (long long)largest_size);
    void *map = mmap(NULL, (size_t)largest_size, PROT_READ, MAP_PRIVATE, fd, 0);
    CHECK(map != MAP_FAILED, "mmap %s: %s", largest, strerror(errno));
    if (map != MAP_FAILED) {
        CHECK(memcmp(map, buf, (size_t)largest_size) == 0, "mmap of %s differs from read", largest);
        munmap(map, (size_t)largest_size);
    }
    off_t mid = largest_size / 2 + 123;
    char part[100];
    CHECK(lseek(fd, mid, SEEK_SET) == mid && read(fd, part, sizeof part) == (ssize_t)sizeof part &&
              memcmp(part, buf + mid, sizeof part) == 0,
          "read at offset %lld", (long long)mid);
    free(buf);
    close(fd);
    printf("isotest: mmap and offset reads of %lld bytes match\n", (long long)largest_size);
}

static void check_readonly(const char *root)
{
    char path[1024];
    snprintf(path, sizeof path, "%s/new-file", root);
    CHECK(open(path, O_WRONLY | O_CREAT, 0644) < 0 && errno == EROFS, "create gave %s", strerror(errno));
    snprintf(path, sizeof path, "%s/new-dir", root);
    CHECK(mkdir(path, 0755) < 0 && errno == EROFS, "mkdir gave %s", strerror(errno));
    if (largest_size > 0) {
        CHECK(open(largest, O_WRONLY) < 0 && errno == EROFS, "open for writing gave %s", strerror(errno));
        CHECK(unlink(largest) < 0 && errno == EROFS, "unlink gave %s", strerror(errno));
    }
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: isotest DIR\n");
        return 2;
    }
    walk(argv[1], "");
    check_map();
    check_readonly(argv[1]);
    printf("isotest: %d entries, %d failures\n", entries, failures);
    return failures ? 1 : 0;
}
