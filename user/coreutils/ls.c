/* ls: list directory contents. -l prints type, size and inode number, -a
 * includes names starting with a dot. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <errno.h>

static int longfmt, all;

static char type_char(unsigned mode)
{
    if (S_ISDIR(mode)) return 'd';
    if (S_ISCHR(mode)) return 'c';
    if (S_ISBLK(mode)) return 'b';
    if (S_ISFIFO(mode)) return 'p';
    return '-';
}

static int cmp_names(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static void print_entry(const char *dir, const char *name)
{
    if (!longfmt) {
        printf("%s\n", name);
        return;
    }
    char path[512];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    struct stat st;
    if (stat(path, &st) < 0) {
        printf("?         ?  %s\n", name);
        return;
    }
    printf("%c %8ld %4lu %s\n", type_char(st.st_mode), (long)st.st_size,
           (unsigned long)st.st_ino, name);
}

static int list(const char *path)
{
    struct stat st;
    if (stat(path, &st) < 0) {
        fprintf(stderr, "ls: %s: %s\n", path, strerror(errno));
        return 1;
    }
    if (!S_ISDIR(st.st_mode)) {
        print_entry(".", path);
        return 0;
    }
    DIR *d = opendir(path);
    if (!d) {
        fprintf(stderr, "ls: %s: %s\n", path, strerror(errno));
        return 1;
    }
    char *names[256];
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) && n < 256) {
        if (e->d_name[0] == '.' && !all)
            continue;
        names[n++] = strdup(e->d_name);
    }
    closedir(d);
    qsort(names, (size_t)n, sizeof names[0], cmp_names);
    for (int i = 0; i < n; i++) {
        print_entry(path, names[i]);
        free(names[i]);
    }
    return 0;
}

int main(int argc, char **argv)
{
    int first = 1;
    while (first < argc && argv[first][0] == '-') {
        for (const char *o = argv[first] + 1; *o; o++) {
            if (*o == 'l')
                longfmt = 1;
            else if (*o == 'a')
                all = 1;
            else {
                fprintf(stderr, "ls: unknown option -%c\n", *o);
                return 2;
            }
        }
        first++;
    }
    if (first >= argc)
        return list(".");
    int status = 0;
    for (int i = first; i < argc; i++) {
        struct stat st;
        if (argc - first > 1 && stat(argv[i], &st) == 0 && S_ISDIR(st.st_mode))
            printf("%s:\n", argv[i]);
        status |= list(argv[i]);
    }
    return status;
}
