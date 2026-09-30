/* find: recursive file search with the common name/type/depth actions.
 * Symbolic links are reported as links and not followed, as with the
 * POSIX default -P, so a link to a directory is neither entered nor
 * matched by -type d; -type l selects links. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/wait.h>

#define PATH_SIZE 1024
#define EXEC_ARGS 64

static const char *name_pattern;
static int wanted_type;
static int min_depth;
static int max_depth = -1;
static int print_zero;
static char **exec_args;
static int exec_count;
static int had_error;

static int glob(const char *p, const char *s)
{
    while (*p) {
        if (*p == '*') {
            while (*p == '*')
                p++;
            if (!*p)
                return 1;
            do {
                if (glob(p, s))
                    return 1;
            } while (*s++);
            return 0;
        }
        if (*p == '?') {
            if (!*s++)
                return 0;
            p++;
            continue;
        }
        if (*p == '[') {
            int negate = p[1] == '!' || p[1] == '^';
            if (negate)
                p++;
            int match = 0;
            p++;
            while (*p && *p != ']') {
                int lo = (unsigned char)*p++;
                int hi = lo;
                if (*p == '-' && p[1] && p[1] != ']') {
                    p++;
                    hi = (unsigned char)*p++;
                }
                if ((unsigned char)*s >= lo && (unsigned char)*s <= hi)
                    match = 1;
            }
            if (*p == ']')
                p++;
            if (!*s || match == negate)
                return 0;
            s++;
            continue;
        }
        if (*p++ != *s++)
            return 0;
    }
    return *s == '\0';
}

static const char *base_name(const char *path)
{
    const char *p = strrchr(path, '/');
    return p && p[1] ? p + 1 : path;
}

static int replace_path(const char *arg, const char *path, char *out, size_t size)
{
    size_t n = 0;
    while (*arg) {
        if (arg[0] == '{' && arg[1] == '}') {
            size_t m = strlen(path);
            if (n + m >= size)
                return -1;
            memcpy(out + n, path, m);
            n += m;
            arg += 2;
        } else {
            if (n + 1 >= size)
                return -1;
            out[n++] = *arg++;
        }
    }
    out[n] = '\0';
    return 0;
}

static int run_exec(const char *path)
{
    char storage[EXEC_ARGS][PATH_SIZE];
    char *av[EXEC_ARGS + 1];
    for (int i = 0; i < exec_count; i++) {
        if (replace_path(exec_args[i], path, storage[i], sizeof storage[i]) < 0) {
            fprintf(stderr, "find: expanded argument is too long\n");
            return 0;
        }
        av[i] = storage[i];
    }
    av[exec_count] = NULL;
    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "find: fork: %s\n", strerror(errno));
        return 0;
    }
    if (pid == 0) {
        execvp(av[0], av);
        fprintf(stderr, "find: %s: %s\n", av[0], strerror(errno));
        _exit(127);
    }
    int status;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
        ;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static int matches(const char *path, const struct stat *st, int depth)
{
    if (depth < min_depth)
        return 0;
    if (name_pattern && !glob(name_pattern, base_name(path)))
        return 0;
    if (wanted_type == 'd' && !S_ISDIR(st->st_mode))
        return 0;
    if (wanted_type == 'f' && !S_ISREG(st->st_mode))
        return 0;
    if (wanted_type == 'l' && !S_ISLNK(st->st_mode))
        return 0;
    return 1;
}

static void visit(const char *path, int depth)
{
    struct stat st;
    if (lstat(path, &st) < 0) {
        fprintf(stderr, "find: %s: %s\n", path, strerror(errno));
        had_error = 1;
        return;
    }
    if (matches(path, &st, depth)) {
        if (exec_count)
            run_exec(path);
        else if (print_zero)
            fwrite(path, 1, strlen(path) + 1, stdout);
        else
            printf("%s\n", path);
    }
    if (!S_ISDIR(st.st_mode) || (max_depth >= 0 && depth >= max_depth))
        return;
    DIR *d = opendir(path);
    if (!d) {
        fprintf(stderr, "find: %s: %s\n", path, strerror(errno));
        had_error = 1;
        return;
    }
    struct dirent *e;
    while ((e = readdir(d))) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        char child[PATH_SIZE];
        int n = snprintf(child, sizeof child, "%s%s%s", path,
                         path[0] && path[strlen(path) - 1] == '/' ? "" : "/", e->d_name);
        if (n < 0 || (size_t)n >= sizeof child) {
            fprintf(stderr, "find: path too long: %s/%s\n", path, e->d_name);
            had_error = 1;
            continue;
        }
        visit(child, depth + 1);
    }
    closedir(d);
}

static int number(const char *option, const char *s)
{
    char *end;
    long n = strtol(s, &end, 10);
    if (!*s || *end || n < 0) {
        fprintf(stderr, "find: %s requires a nonnegative number\n", option);
        exit(2);
    }
    return (int)n;
}

int main(int argc, char **argv)
{
    int first_path = 1, expression = 1;
    while (expression < argc && argv[expression][0] != '-')
        expression++;
    if (expression == 1)
        first_path = 0;
    for (int i = expression; i < argc; i++) {
        if (strcmp(argv[i], "-name") == 0 && i + 1 < argc)
            name_pattern = argv[++i];
        else if (strcmp(argv[i], "-type") == 0 && i + 1 < argc &&
                 strchr("fdl", argv[i + 1][0]) && argv[i + 1][0] && !argv[i + 1][1])
            wanted_type = argv[++i][0];
        else if (strcmp(argv[i], "-mindepth") == 0 && i + 1 < argc)
            min_depth = number("-mindepth", argv[++i]);
        else if (strcmp(argv[i], "-maxdepth") == 0 && i + 1 < argc)
            max_depth = number("-maxdepth", argv[++i]);
        else if (strcmp(argv[i], "-print") == 0)
            print_zero = 0;
        else if (strcmp(argv[i], "-print0") == 0)
            print_zero = 1;
        else if (strcmp(argv[i], "-exec") == 0) {
            int start = ++i;
            while (i < argc && strcmp(argv[i], ";") != 0)
                i++;
            if (i == argc || i == start || i - start > EXEC_ARGS) {
                fprintf(stderr, "find: -exec requires a command terminated by ;\n");
                return 2;
            }
            exec_args = argv + start;
            exec_count = i - start;
        } else {
            fprintf(stderr, "find: unsupported expression: %s\n", argv[i]);
            fprintf(stderr, "usage: find [path...] [-name pattern] [-type f|d|l] "
                            "[-mindepth n] [-maxdepth n] [-print0] [-exec command {} ;]\n");
            return 2;
        }
    }
    if (!first_path) {
        visit(".", 0);
    } else {
        for (int i = first_path; i < expression; i++)
            visit(argv[i], 0);
    }
    return had_error;
}
