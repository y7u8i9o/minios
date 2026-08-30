/* printenv: print all variables, or the value of the named ones. */
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv, char **envp)
{
    if (argc == 1) {
        for (char **e = envp; *e; e++)
            puts(*e);
        return 0;
    }
    int status = 0;
    for (int i = 1; i < argc; i++) {
        const char *v = getenv(argv[i]);
        if (v)
            puts(v);
        else
            status = 1;
    }
    return status;
}
