/* xargs: construct command lines from standard input. Supports quoting,
 * NUL input, bounded batches, replacement mode and tracing. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/wait.h>

#define MAX_VECTOR 128
#define DEFAULT_SIZE 4096

static int nul_input, no_run_empty, trace;
static int max_args = MAX_VECTOR - 1;
static int max_size = DEFAULT_SIZE;
static const char *replace;
static int command_status;

static void show(char **av)
{
    for (int i = 0; av[i]; i++)
        fprintf(stderr, "%s%s", i ? " " : "", av[i]);
    fputc('\n', stderr);
}

static void execute(char **av)
{
    if (trace)
        show(av);
    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "xargs: fork: %s\n", strerror(errno));
        command_status = 1;
        return;
    }
    if (pid == 0) {
        execvp(av[0], av);
        fprintf(stderr, "xargs: %s: %s\n", av[0], strerror(errno));
        _exit(errno == ENOENT ? 127 : 126);
    }
    int status;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
        ;
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        int code = WIFEXITED(status) ? WEXITSTATUS(status) : 125;
        command_status = code == 126 || code == 127 ? code : 123;
    }
}

static char *replace_one(const char *s, const char *item)
{
    size_t repl = strlen(replace), value = strlen(item), count = 0;
    const char *p = s;
    while ((p = strstr(p, replace))) {
        count++;
        p += repl;
    }
    size_t size = strlen(s) + count * value - count * repl + 1;
    char *out = malloc(size);
    if (!out)
        return NULL;
    char *d = out;
    p = s;
    for (;;) {
        const char *at = strstr(p, replace);
        if (!at) {
            strcpy(d, p);
            break;
        }
        memcpy(d, p, (size_t)(at - p));
        d += at - p;
        memcpy(d, item, value);
        d += value;
        p = at + repl;
    }
    return out;
}

static void execute_replaced(char **base, int nbase, const char *item)
{
    char *av[MAX_VECTOR];
    for (int i = 0; i < nbase; i++) {
        av[i] = replace_one(base[i], item);
        if (!av[i]) {
            fprintf(stderr, "xargs: out of memory\n");
            exit(1);
        }
    }
    av[nbase] = NULL;
    execute(av);
    for (int i = 0; i < nbase; i++)
        free(av[i]);
}

/* Return the next shell-like word. In replacement mode a whole input
 * line is one item. */
static char *next_item(void)
{
    int c, quote = 0, escaped = 0, started = 0;
    size_t n = 0, cap = 64;
    char *s = malloc(cap);
    if (!s)
        return NULL;
    while ((c = fgetc(stdin)) != EOF) {
        int separator = nul_input ? c == 0 : replace ? c == '\n' :
                        (!quote && !escaped && (c == ' ' || c == '\t' || c == '\n'));
        if (separator) {
            if (started)
                break;
            continue;
        }
        started = 1;
        if (!nul_input && !replace) {
            if (escaped) {
                escaped = 0;
            } else if (c == '\\' && quote != '\'') {
                escaped = 1;
                continue;
            } else if ((c == '\'' || c == '"')) {
                if (!quote) {
                    quote = c;
                    continue;
                }
                if (quote == c) {
                    quote = 0;
                    continue;
                }
            }
        }
        if (n + 1 >= cap) {
            cap *= 2;
            char *p = realloc(s, cap);
            if (!p) {
                free(s);
                return NULL;
            }
            s = p;
        }
        s[n++] = (char)c;
    }
    if (!started) {
        free(s);
        return NULL;
    }
    if (quote || escaped) {
        fprintf(stderr, "xargs: unmatched quote or trailing backslash\n");
        free(s);
        command_status = 1;
        return NULL;
    }
    if (replace) {
        while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r'))
            n--;
        size_t begin = 0;
        while (begin < n && (s[begin] == ' ' || s[begin] == '\t'))
            begin++;
        if (begin) {
            memmove(s, s + begin, n - begin);
            n -= begin;
        }
    }
    s[n] = '\0';
    return s;
}

static int option_number(const char *name, const char *s)
{
    char *end;
    long n = strtol(s, &end, 10);
    if (!*s || *end || n <= 0) {
        fprintf(stderr, "xargs: %s requires a positive number\n", name);
        exit(1);
    }
    return (int)n;
}

int main(int argc, char **argv)
{
    int i = 1;
    for (; i < argc; i++) {
        if (strcmp(argv[i], "--") == 0) {
            i++;
            break;
        } else if (strcmp(argv[i], "-0") == 0) {
            nul_input = 1;
        } else if (strcmp(argv[i], "-r") == 0) {
            no_run_empty = 1;
        } else if (strcmp(argv[i], "-t") == 0) {
            trace = 1;
        } else if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
            max_args = option_number("-n", argv[++i]);
        } else if (strncmp(argv[i], "-n", 2) == 0 && argv[i][2]) {
            max_args = option_number("-n", argv[i] + 2);
        } else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
            max_size = option_number("-s", argv[++i]);
        } else if (strncmp(argv[i], "-s", 2) == 0 && argv[i][2]) {
            max_size = option_number("-s", argv[i] + 2);
        } else if (strcmp(argv[i], "-I") == 0 && i + 1 < argc) {
            replace = argv[++i];
            if (!*replace) {
                fprintf(stderr, "xargs: -I requires a nonempty replacement string\n");
                return 1;
            }
        } else if (argv[i][0] == '-' && argv[i][1]) {
            fprintf(stderr, "usage: xargs [-0rt] [-n count] [-s size] [-I replace] [command [args...]]\n");
            return 1;
        } else {
            break;
        }
    }

    char *default_cmd[] = { "echo", NULL };
    char **base = i < argc ? argv + i : default_cmd;
    int nbase = i < argc ? argc - i : 1;
    if (replace) {
        int any = 0;
        char *item;
        while ((item = next_item())) {
            any = 1;
            execute_replaced(base, nbase, item);
            free(item);
        }
        if (!any && !no_run_empty)
            execute_replaced(base, nbase, "");
        return command_status;
    }

    char *av[MAX_VECTOR];
    int used = nbase;
    int bytes = 0, any = 0;
    if (nbase >= MAX_VECTOR) {
        fprintf(stderr, "xargs: initial command is too long\n");
        return 1;
    }
    for (int k = 0; k < nbase; k++) {
        av[k] = base[k];
        bytes += (int)strlen(base[k]) + 1;
    }
    char *item;
    while ((item = next_item())) {
        int size = (int)strlen(item) + 1;
        if (size + bytes > max_size && used == nbase) {
            fprintf(stderr, "xargs: argument exceeds the -s limit\n");
            free(item);
            return 1;
        }
        if (used > nbase && (used - nbase >= max_args || size + bytes > max_size)) {
            av[used] = NULL;
            execute(av);
            for (int k = nbase; k < used; k++)
                free(av[k]);
            used = nbase;
            bytes = 0;
            for (int k = 0; k < nbase; k++)
                bytes += (int)strlen(base[k]) + 1;
        }
        av[used++] = item;
        bytes += size;
        any = 1;
        if (used == MAX_VECTOR - 1) {
            av[used] = NULL;
            execute(av);
            for (int k = nbase; k < used; k++)
                free(av[k]);
            used = nbase;
            bytes = 0;
            for (int k = 0; k < nbase; k++)
                bytes += (int)strlen(base[k]) + 1;
        }
    }
    if (used > nbase || (!any && !no_run_empty)) {
        av[used] = NULL;
        execute(av);
    }
    for (int k = nbase; k < used; k++)
        free(av[k]);
    return command_status;
}
