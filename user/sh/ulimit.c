#include "sh.h"
struct ulimit_opt {
    char letter;
    int resource;
    unsigned long unit;
    const char *desc;
};
static const struct ulimit_opt ulimit_opts[] = {
    { 'c', RLIMIT_CORE, 1024, "core file size (KiB)" },
    { 'd', RLIMIT_DATA, 1024, "data seg size (KiB)" },
    { 'f', RLIMIT_FSIZE, 512, "file size (blocks)" },
    { 'n', RLIMIT_NOFILE, 1, "open files" },
    { 's', RLIMIT_STACK, 1024, "stack size (KiB)" },
    { 't', RLIMIT_CPU, 1, "cpu time (seconds)" },
    { 'u', RLIMIT_NPROC, 1, "max user processes" },
    { 'v', RLIMIT_AS, 1024, "virtual memory (KiB)" },
};

static void ulimit_print(const struct ulimit_opt *o, int hard, int with_desc)
{
    struct rlimit rl;
    if (getrlimit(o->resource, &rl) < 0) {
        fprintf(stderr, "ulimit: %s\n", strerror(errno));
        return;
    }
    unsigned long v = hard ? rl.rlim_max : rl.rlim_cur;
    if (with_desc)
        printf("%-24s (-%c) ", o->desc, o->letter);
    if (v == RLIM_INFINITY)
        printf("unlimited\n");
    else
        printf("%lu\n", v / o->unit);
}

int builtin_ulimit(char **argv)
{
    int hard = 0, soft = 0, all = 0;
    const struct ulimit_opt *sel = NULL;
    int i = 1;
    for (; argv[i] && argv[i][0] == '-' && argv[i][1]; i++) {
        for (const char *c = argv[i] + 1; *c; c++) {
            if (*c == 'H') { hard = 1; continue; }
            if (*c == 'S') { soft = 1; continue; }
            if (*c == 'a') { all = 1; continue; }
            const struct ulimit_opt *o = NULL;
            for (size_t k = 0; k < sizeof ulimit_opts / sizeof ulimit_opts[0]; k++)
                if (ulimit_opts[k].letter == *c)
                    o = &ulimit_opts[k];
            if (!o) {
                fprintf(stderr, "ulimit: -%c: invalid option\n", *c);
                fprintf(stderr, "usage: ulimit [-HS] [-acdfnstuv] [value|unlimited]\n");
                return 2;
            }
            sel = o;
        }
    }
    if (all) {
        for (size_t k = 0; k < sizeof ulimit_opts / sizeof ulimit_opts[0]; k++)
            ulimit_print(&ulimit_opts[k], hard, 1);
        return 0;
    }
    if (!sel)
        sel = &ulimit_opts[2];              /* -f, as in other shells */
    if (!argv[i]) {
        ulimit_print(sel, hard, 0);
        return 0;
    }
    struct rlimit rl;
    if (getrlimit(sel->resource, &rl) < 0) {
        fprintf(stderr, "ulimit: %s\n", strerror(errno));
        return 1;
    }
    unsigned long v;
    if (strcmp(argv[i], "unlimited") == 0) {
        v = RLIM_INFINITY;
    } else {
        char *end;
        v = strtoul(argv[i], &end, 10);
        if (*end || end == argv[i]) {
            fprintf(stderr, "ulimit: %s: invalid number\n", argv[i]);
            return 1;
        }
        v *= sel->unit;
    }
    if (!hard && !soft)
        hard = soft = 1;                    /* both, like other shells */
    if (hard)
        rl.rlim_max = v;
    if (soft)
        rl.rlim_cur = v;
    if (rl.rlim_cur > rl.rlim_max)
        rl.rlim_cur = rl.rlim_max;
    if (setrlimit(sel->resource, &rl) < 0) {
        fprintf(stderr, "ulimit: %s\n", strerror(errno));
        return 1;
    }
    return 0;
}


