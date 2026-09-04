/* prlimit: show or change the resource limits of a process.
 * usage: prlimit [-p pid] [RESOURCE[=soft[:hard]]]...
 * Resources: cpu fsize data stack core rss nproc nofile memlock as.
 * A value may be "unlimited". Without arguments every limit is listed. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/resource.h>

static const struct { const char *name; int res; const char *desc; const char *unit; } table[] = {
    { "cpu", RLIMIT_CPU, "max CPU time", "seconds" },
    { "fsize", RLIMIT_FSIZE, "max file size", "bytes" },
    { "data", RLIMIT_DATA, "max data size", "bytes" },
    { "stack", RLIMIT_STACK, "max stack size", "bytes" },
    { "core", RLIMIT_CORE, "max core file size", "bytes" },
    { "rss", RLIMIT_RSS, "max resident set size", "bytes" },
    { "nproc", RLIMIT_NPROC, "max number of processes", "processes" },
    { "nofile", RLIMIT_NOFILE, "max number of open files", "files" },
    { "memlock", RLIMIT_MEMLOCK, "max locked memory", "bytes" },
    { "as", RLIMIT_AS, "address space limit", "bytes" },
};
#define NRES (sizeof table / sizeof table[0])

static void fmt(unsigned long v, char *buf, size_t n)
{
    if (v == RLIM_INFINITY)
        snprintf(buf, n, "unlimited");
    else
        snprintf(buf, n, "%lu", v);
}

static int parse_value(const char *s, unsigned long *out)
{
    if (strcmp(s, "unlimited") == 0) {
        *out = RLIM_INFINITY;
        return 0;
    }
    char *end;
    *out = strtoul(s, &end, 10);
    return *end || end == s ? -1 : 0;
}

static int show(pid_t pid, int only)
{
    printf("%-8s %-26s %10s %10s %s\n", "RESOURCE", "DESCRIPTION", "SOFT", "HARD", "UNITS");
    for (size_t i = 0; i < NRES; i++) {
        if (only >= 0 && (int)i != only)
            continue;
        struct rlimit rl;
        if (prlimit(pid, table[i].res, NULL, &rl) < 0) {
            fprintf(stderr, "prlimit: pid %d: %s\n", pid, strerror(errno));
            return 1;
        }
        char soft[24], hard[24];
        fmt(rl.rlim_cur, soft, sizeof soft);
        fmt(rl.rlim_max, hard, sizeof hard);
        printf("%-8s %-26s %10s %10s %s\n", table[i].name, table[i].desc, soft, hard, table[i].unit);
    }
    return 0;
}

int main(int argc, char **argv)
{
    pid_t pid = 0;
    int i = 1;
    if (i < argc && strcmp(argv[i], "-p") == 0) {
        if (i + 1 >= argc) {
            fprintf(stderr, "usage: prlimit [-p pid] [RESOURCE[=soft[:hard]]]...\n");
            return 2;
        }
        pid = atoi(argv[i + 1]);
        i += 2;
    }
    if (i >= argc)
        return show(pid, -1);
    int status = 0;
    for (; i < argc; i++) {
        char name[16];
        const char *eq = strchr(argv[i], '=');
        size_t len = eq ? (size_t)(eq - argv[i]) : strlen(argv[i]);
        if (len >= sizeof name)
            len = sizeof name - 1;
        memcpy(name, argv[i], len);
        name[len] = '\0';
        int res = -1;
        for (size_t k = 0; k < NRES; k++)
            if (strcmp(table[k].name, name) == 0)
                res = (int)k;
        if (res < 0) {
            fprintf(stderr, "prlimit: unknown resource %s\n", name);
            return 2;
        }
        if (!eq) {
            status |= show(pid, res);
            continue;
        }
        struct rlimit rl;
        if (prlimit(pid, table[res].res, NULL, &rl) < 0) {
            fprintf(stderr, "prlimit: pid %d: %s\n", pid, strerror(errno));
            return 1;
        }
        char *colon = strchr(eq + 1, ':');
        unsigned long soft, hard = rl.rlim_max;
        if (colon) {
            *colon = '\0';
            if (parse_value(colon + 1, &hard) < 0) {
                fprintf(stderr, "prlimit: bad hard limit\n");
                return 2;
            }
        }
        if (parse_value(eq + 1, &soft) < 0) {
            fprintf(stderr, "prlimit: bad soft limit\n");
            return 2;
        }
        rl.rlim_cur = soft;
        rl.rlim_max = hard;
        if (prlimit(pid, table[res].res, &rl, NULL) < 0) {
            fprintf(stderr, "prlimit: %s: %s\n", name, strerror(errno));
            status = 1;
        }
    }
    return status;
}
