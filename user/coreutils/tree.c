/* tree: print a directory tree. A symbolic link is printed as
 * "name -> target" and not entered. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <errno.h>
#include <term.h>

static int all, directories, color, maxdepth = 64, errors;
static unsigned long dir_count, file_count;

static int compare(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static void walk(const char *path, const char *prefix, int depth)
{
    if (depth >= maxdepth)
        return;
    DIR *dir = opendir(path);
    if (!dir) {
        fprintf(stderr, "tree: %s: %s\n", path, strerror(errno));
        errors = 1;
        return;
    }
    char **names = NULL;
    size_t count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..") || (!all && entry->d_name[0] == '.'))
            continue;
        if (directories && entry->d_type != DT_DIR)
            continue;
        char **next = realloc(names, (count + 1) * sizeof *next);
        if (!next) {
            errors = 1;
            break;
        }
        names = next;
        names[count] = strdup(entry->d_name);
        if (!names[count]) {
            errors = 1;
            break;
        }
        count++;
    }
    closedir(dir);
    qsort(names, count, sizeof *names, compare);
    for (size_t i = 0; i < count; i++) {
        size_t size = strlen(path) + strlen(names[i]) + 2;
        char *full = malloc(size);
        if (!full) {
            errors = 1;
            free(names[i]);
            continue;
        }
        snprintf(full, size, "%s/%s", path, names[i]);
        struct stat st;
        if (lstat(full, &st) < 0) {
            errors = 1;
            free(full);
            free(names[i]);
            continue;
        }
        int isdir = S_ISDIR(st.st_mode);
        int islink = S_ISLNK(st.st_mode);
        int sgr = isdir ? 34 : islink ? 36 : (st.st_mode & 0111) ? 32 : 0;
        printf("%s%s", prefix, i + 1 == count ? "`-- " : "|-- ");
        if (color && sgr)
            fputs(term_sgr(sgr), stdout);
        fputs(names[i], stdout);
        if (color && sgr)
            fputs(term_sgr(0), stdout);
        char target[1024];
        ssize_t n = islink ? readlink(full, target, sizeof target - 1) : -1;
        if (n >= 0)
            printf(" -> %.*s", (int)n, target);
        putchar('\n');
        if (isdir) {
            dir_count++;
            size_t length = strlen(prefix);
            char *indent = malloc(length + 5);
            if (indent) {
                snprintf(indent, length + 5, "%s%s", prefix, i + 1 == count ? "    " : "|   ");
                walk(full, indent, depth + 1);
                free(indent);
            } else {
                errors = 1;
            }
        } else {
            file_count++;
        }
        free(full);
        free(names[i]);
    }
    free(names);
}

int main(int argc, char **argv)
{
    color = term_use_color(1);
    int first = 1;
    for (; first < argc && argv[first][0] == '-' && argv[first][1]; first++) {
        if (!strcmp(argv[first], "--")) {
            first++;
            break;
        }
        if (!strcmp(argv[first], "-L") && first + 1 < argc) {
            char *end;
            long limit = strtol(argv[++first], &end, 10);
            if (*end || limit < 1 || limit > 128)
                return 2;
            maxdepth = (int)limit;
        } else if (!strcmp(argv[first], "-a")) {
            all = 1;
        } else if (!strcmp(argv[first], "-d")) {
            directories = 1;
        } else {
            fprintf(stderr, "usage: tree [-a] [-d] [-L depth] [directory...]\n");
            return 2;
        }
    }
    if (first == argc) {
        puts(".");
        walk(".", "", 0);
    }
    for (int i = first; i < argc; i++) {
        puts(argv[i]);
        walk(argv[i], "", 0);
    }
    printf("\n%lu directories, %lu files\n", dir_count, file_count);
    return errors;
}
