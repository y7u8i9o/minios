#include "sh.h"
#include <sys/stat.h>

static int evaluate(int argc, char **argv, int *error)
{
    if (!argc)
        return 0;
    if (argc == 1)
        return *argv[0] != 0;
    if (!strcmp(argv[0], "!"))
        return !evaluate(argc - 1, argv + 1, error);
    if (argc == 2) {
        struct stat st;
        if (!strcmp(argv[0], "-z"))
            return !*argv[1];
        if (!strcmp(argv[0], "-n"))
            return *argv[1] != 0;
        if (!strcmp(argv[0], "-t"))
            return isatty(atoi(argv[1]));
        if (strchr("efdbcrwxs", argv[0][1]) && argv[0][0] == '-' && !argv[0][2]) {
            if (stat(argv[1], &st) < 0)
                return 0;
            switch (argv[0][1]) {
            case 'e':
                return 1;
            case 'f':
                return S_ISREG(st.st_mode);
            case 'd':
                return S_ISDIR(st.st_mode);
            case 'b':
                return S_ISBLK(st.st_mode);
            case 'c':
                return S_ISCHR(st.st_mode);
            case 'r':
                return (st.st_mode & 0444) != 0;
            case 'w':
                return (st.st_mode & 0222) != 0;
            case 'x':
                return (st.st_mode & 0111) != 0;
            case 's':
                return st.st_size > 0;
            }
        }
    }
    if (argc == 3) {
        const char *op = argv[1];
        int comparison = strcmp(argv[0], argv[2]);
        if (!strcmp(op, "="))
            return comparison == 0;
        if (!strcmp(op, "!="))
            return comparison != 0;
        if (!strcmp(op, "<"))
            return comparison < 0;
        if (!strcmp(op, ">"))
            return comparison > 0;
        if (!strcmp(op, "-eq") || !strcmp(op, "-ne") || !strcmp(op, "-lt") || !strcmp(op, "-le") ||
            !strcmp(op, "-gt") || !strcmp(op, "-ge")) {
            char *end_a, *end_b;
            long a = strtol(argv[0], &end_a, 10), b = strtol(argv[2], &end_b, 10);
            if (!*argv[0] || !*argv[2] || *end_a || *end_b) {
                *error = 1;
                return 0;
            }
            if (!strcmp(op, "-eq"))
                return a == b;
            if (!strcmp(op, "-ne"))
                return a != b;
            if (!strcmp(op, "-lt"))
                return a < b;
            if (!strcmp(op, "-le"))
                return a <= b;
            if (!strcmp(op, "-gt"))
                return a > b;
            return a >= b;
        }
    }
    int depth = 0;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "("))
            depth++;
        if (!strcmp(argv[i], ")"))
            depth--;
        if (!depth && (!strcmp(argv[i], "-a") || !strcmp(argv[i], "-o"))) {
            int a = evaluate(i, argv, error);
            int b = evaluate(argc - i - 1, argv + i + 1, error);
            return argv[i][1] == 'a' ? a && b : a || b;
        }
    }
    if (argc >= 3 && !strcmp(argv[0], "(") && !strcmp(argv[argc - 1], ")"))
        return evaluate(argc - 2, argv + 1, error);
    *error = 1;
    return 0;
}

int builtin_test(int argc, char **argv)
{
    if (!strcmp(argv[0], "[")) {
        if (argc < 2 || strcmp(argv[argc - 1], "]")) {
            fprintf(stderr, "[: missing ]\n");
            return 2;
        }
        argc--;
    }
    int error = 0;
    int result = evaluate(argc - 1, argv + 1, &error);
    if (error) {
        fprintf(stderr, "test: invalid expression\n");
        return 2;
    }
    return !result;
}
