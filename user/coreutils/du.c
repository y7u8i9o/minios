/* du: disk usage of files and directories in kB. -s prints only totals. */
#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>

static int summary;

static long usage(const char *path, int top)
{
    struct stat st;
    if (stat(path, &st) < 0) {
        fprintf(stderr, "du: %s: %s\n", path, strerror(errno));
        return 0;
    }
    long kb = (st.st_size + 1023) / 1024;
    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(path);
        if (d) {
            struct dirent *e;
            while ((e = readdir(d))) {
                if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
                    continue;
                char child[512];
                snprintf(child, sizeof child, "%s%s%s", path,
                         path[strlen(path) - 1] == '/' ? "" : "/", e->d_name);
                kb += usage(child, 0);
            }
            closedir(d);
        }
        if (!summary || top)
            printf("%ld\t%s\n", kb, path);
    } else if (top) {
        printf("%ld\t%s\n", kb, path);
    }
    return kb;
}

int main(int argc, char **argv)
{
    int i = 1;
    if (i < argc && strcmp(argv[i], "-s") == 0) {
        summary = 1;
        i++;
    }
    if (i >= argc)
        usage(".", 1);
    for (; i < argc; i++)
        usage(argv[i], 1);
    return 0;
}
