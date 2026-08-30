/* test: evaluate a condition and exit 0 when true. Supports string
 * comparison (= !=), integer comparison (-eq -ne -lt -le -gt -ge),
 * -z -n, file tests (-e -f -d), and ! for negation. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static int eval(int argc, char **argv)
{
    if (argc == 0)
        return 0;
    if (strcmp(argv[0], "!") == 0)
        return !eval(argc - 1, argv + 1);
    if (argc == 1)
        return argv[0][0] != '\0';
    if (argc == 2) {
        struct stat st;
        if (strcmp(argv[0], "-z") == 0) return argv[1][0] == '\0';
        if (strcmp(argv[0], "-n") == 0) return argv[1][0] != '\0';
        if (strcmp(argv[0], "-e") == 0) return stat(argv[1], &st) == 0;
        if (strcmp(argv[0], "-f") == 0) return stat(argv[1], &st) == 0 && S_ISREG(st.st_mode);
        if (strcmp(argv[0], "-d") == 0) return stat(argv[1], &st) == 0 && S_ISDIR(st.st_mode);
        return 0;
    }
    if (argc == 3) {
        const char *op = argv[1];
        long a = atol(argv[0]), b = atol(argv[2]);
        if (strcmp(op, "=") == 0) return strcmp(argv[0], argv[2]) == 0;
        if (strcmp(op, "!=") == 0) return strcmp(argv[0], argv[2]) != 0;
        if (strcmp(op, "-eq") == 0) return a == b;
        if (strcmp(op, "-ne") == 0) return a != b;
        if (strcmp(op, "-lt") == 0) return a < b;
        if (strcmp(op, "-le") == 0) return a <= b;
        if (strcmp(op, "-gt") == 0) return a > b;
        if (strcmp(op, "-ge") == 0) return a >= b;
    }
    fprintf(stderr, "test: invalid expression\n");
    return 0;
}

int main(int argc, char **argv)
{
    int n = argc - 1;
    if (n > 0 && strcmp(argv[n], "]") == 0)
        n--;
    return eval(n, argv + 1) ? 0 : 1;
}
