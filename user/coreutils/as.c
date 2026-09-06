/* as: assemble with the assembler of tcc under the conventional name.
 *
 *     as [-o file] [-I dir] [-g] [--64] file
 *
 * The file is assembled by /bin/tcc -c. The output is a.out without -o. */
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define MAX_ARGS 64

int main(int argc, char **argv)
{
    char *args[MAX_ARGS];
    int n = 0;
    const char *out = "a.out";
    const char *source = NULL;
    args[n++] = "tcc";
    args[n++] = "-c";
    for (int i = 1; i < argc; i++) {
        char *a = argv[i];
        if (strcmp(a, "-o") == 0 && i + 1 < argc) {
            out = argv[++i];
        } else if (strncmp(a, "-o", 2) == 0 && a[2] != '\0') {
            out = a + 2;
        } else if (strcmp(a, "-I") == 0 && i + 1 < argc && n < MAX_ARGS - 4) {
            args[n++] = "-I";
            args[n++] = argv[++i];
        } else if (strncmp(a, "-I", 2) == 0 && n < MAX_ARGS - 4) {
            args[n++] = a;
        } else if (strcmp(a, "-g") == 0 || strcmp(a, "--64") == 0 || strcmp(a, "-W") == 0 ||
                   strcmp(a, "--noexecstack") == 0 || strcmp(a, "-c") == 0) {
            continue;
        } else if (strcmp(a, "--version") == 0 || strcmp(a, "-v") == 0) {
            printf("as: the assembler of tcc\n");
            return 0;
        } else if (a[0] == '-' && a[1] != '\0') {
            fprintf(stderr, "as: unsupported option %s\n", a);
            return 1;
        } else if (source == NULL) {
            source = a;
        } else {
            fprintf(stderr, "as: one source file at a time\n");
            return 1;
        }
    }
    if (source == NULL) {
        fprintf(stderr, "usage: as [-o file] [-I dir] file\n");
        return 2;
    }
    args[n++] = "-o";
    args[n++] = (char *)out;
    args[n++] = (char *)source;
    args[n] = NULL;
    execv("/bin/tcc", args);
    perror("as: /bin/tcc");
    return 127;
}
