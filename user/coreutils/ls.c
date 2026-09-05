/* ls: sorted, width-aware directory listings. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <errno.h>
#include <time.h>
#include <term.h>
#include <wchar.h>

struct entry {
    char *name;
    struct stat st;
};
static int longfmt, all, human, columns, reverse, by_time, by_size, directory, classify, color;

static int compare(const void *a, const void *b)
{
    const struct entry *x = a, *y = b;
    int result;
    if (by_time && x->st.st_mtime != y->st.st_mtime)
        result = x->st.st_mtime > y->st.st_mtime ? -1 : 1;
    else if (by_size && x->st.st_size != y->st.st_size)
        result = x->st.st_size > y->st.st_size ? -1 : 1;
    else
        result = strcmp(x->name, y->name);
    return reverse ? -result : result;
}

static char suffix(unsigned mode)
{
    if (S_ISDIR(mode))
        return '/';
    if (S_ISFIFO(mode))
        return '|';
    if (mode & 0111)
        return '*';
    return 0;
}

static int display_width(const char *s)
{
    int width = 0;
    while (*s) {
        wchar_t wc;
        mbstate_t state = {0};
        size_t n = mbrtowc(&wc, s, strlen(s), &state);
        if (n == (size_t)-1 || n == (size_t)-2) {
            s++;
            width++;
        } else {
            int w = wcwidth(wc);
            width += w < 0 ? 1 : w;
            s += n;
        }
    }
    return width;
}

static void print_name(const struct entry *e)
{
    unsigned mode = e->st.st_mode;
    int sgr = 0;
    if (S_ISDIR(mode))
        sgr = 34;
    else if (S_ISCHR(mode) || S_ISBLK(mode))
        sgr = 33;
    else if (S_ISFIFO(mode))
        sgr = 35;
    else if (mode & 0111)
        sgr = 32;
    if (color && sgr)
        fputs(term_sgr(sgr), stdout);
    fputs(e->name, stdout);
    if (classify && suffix(mode))
        putchar(suffix(mode));
    if (color && sgr)
        fputs(term_sgr(0), stdout);
}

static void print_long(const struct entry *e)
{
    unsigned mode = e->st.st_mode;
    char permissions[11] = "----------";
    permissions[0] = S_ISDIR(mode)    ? 'd'
                     : S_ISCHR(mode)  ? 'c'
                     : S_ISBLK(mode)  ? 'b'
                     : S_ISFIFO(mode) ? 'p'
                                      : '-';
    for (int i = 0; i < 9; i++)
        if (mode & (1u << (8 - i)))
            permissions[i + 1] = "rwx"[i % 3];
    char size[32];
    if (human && e->st.st_size >= 1024) {
        double value = (double)e->st.st_size;
        int unit = 0;
        while (value >= 1024 && unit < 5) {
            value /= 1024;
            unit++;
        }
        snprintf(size, sizeof size, "%.1f%c", value, "BKMGTP"[unit]);
    } else {
        snprintf(size, sizeof size, "%ld", (long)e->st.st_size);
    }
    time_t mtime = e->st.st_mtime;
    struct tm tm;
    char date[32] = "?";
    if (localtime_r(&mtime, &tm))
        strftime(date, sizeof date, "%b %d %H:%M", &tm);
    printf("%s %3lu %8s %s ", permissions, (unsigned long)e->st.st_nlink, size, date);
    print_name(e);
    putchar('\n');
}

static int list(const char *path)
{
    struct stat st;
    if (stat(path, &st) < 0) {
        fprintf(stderr, "ls: %s: %s\n", path, strerror(errno));
        return 1;
    }
    if (!S_ISDIR(st.st_mode) || directory) {
        struct entry e = {.name = (char *)path, .st = st};
        if (longfmt) {
            print_long(&e);
        } else {
            print_name(&e);
            putchar('\n');
        }
        return 0;
    }
    DIR *dir = opendir(path);
    if (!dir) {
        fprintf(stderr, "ls: %s: %s\n", path, strerror(errno));
        return 1;
    }
    struct entry *entries = NULL;
    size_t count = 0;
    int status = 0, width = 1;
    struct dirent *entry;
    while ((entry = readdir(dir))) {
        if (!all && entry->d_name[0] == '.')
            continue;
        size_t length = strlen(path) + strlen(entry->d_name) + 2;
        char *full = malloc(length);
        if (!full) {
            status = 1;
            break;
        }
        snprintf(full, length, "%s/%s", path, entry->d_name);
        int found = stat(full, &st) == 0;
        free(full);
        if (!found) {
            status = 1;
            continue;
        }
        struct entry *next = realloc(entries, (count + 1) * sizeof *next);
        if (!next) {
            status = 1;
            break;
        }
        entries = next;
        entries[count].name = strdup(entry->d_name);
        entries[count].st = st;
        if (!entries[count].name) {
            status = 1;
            break;
        }
        int n = display_width(entry->d_name) + (classify && suffix(st.st_mode) != 0);
        if (n > width)
            width = n;
        count++;
    }
    closedir(dir);
    qsort(entries, count, sizeof *entries, compare);
    int ncols = columns && !longfmt ? term_columns(1) / (width + 2) : 1;
    if (ncols < 1)
        ncols = 1;
    size_t rows = (count + (size_t)ncols - 1) / (size_t)ncols;
    for (size_t row = 0; row < rows; row++) {
        for (int col = 0; col < ncols; col++) {
            size_t at = row + (size_t)col * rows;
            if (at >= count)
                break;
            if (longfmt) {
                print_long(&entries[at]);
            } else {
                print_name(&entries[at]);
                if (at + rows < count) {
                    int used =
                        display_width(entries[at].name) + (classify && suffix(entries[at].st.st_mode) != 0);
                    for (int i = used; i < width + 2; i++)
                        putchar(' ');
                }
            }
        }
        if (!longfmt)
            putchar('\n');
    }
    for (size_t i = 0; i < count; i++)
        free(entries[i].name);
    free(entries);
    return status;
}

int main(int argc, char **argv)
{
    columns = isatty(1);
    color = term_use_color(1);
    int first = 1;
    for (; first < argc && argv[first][0] == '-' && argv[first][1]; first++) {
        if (!strcmp(argv[first], "--")) {
            first++;
            break;
        }
        for (const char *p = argv[first] + 1; *p; p++) {
            switch (*p) {
            case '1':
                columns = 0;
                longfmt = 0;
                break;
            case 'C':
                columns = 1;
                longfmt = 0;
                break;
            case 'l':
                longfmt = 1;
                break;
            case 'h':
                human = 1;
                break;
            case 'a':
                all = 1;
                break;
            case 't':
                by_time = 1;
                by_size = 0;
                break;
            case 'S':
                by_size = 1;
                by_time = 0;
                break;
            case 'r':
                reverse = 1;
                break;
            case 'd':
                directory = 1;
                break;
            case 'F':
                classify = 1;
                break;
            default:
                fprintf(stderr, "ls: unknown option -%c\n", *p);
                return 2;
            }
        }
    }
    if (first == argc)
        return list(".");
    int status = 0;
    for (int i = first; i < argc; i++) {
        if (argc - first > 1 && !directory)
            printf("%s%s:\n", i > first ? "\n" : "", argv[i]);
        status |= list(argv[i]);
    }
    return status;
}
