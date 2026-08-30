/* env: print the environment, or run a command with extra NAME=value
 * assignments. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

int main(int argc, char **argv, char **envp)
{
    int i = 1;
    while (i < argc && strchr(argv[i], '=')) {
        char *eq = strchr(argv[i], '=');
        *eq = '\0';
        setenv(argv[i], eq + 1, 1);
        i++;
    }
    if (i >= argc) {
        for (char **e = envp; *e; e++)
            puts(*e);
        return 0;
    }
    execvp(argv[i], argv + i);
    fprintf(stderr, "env: %s: %s\n", argv[i], strerror(errno));
    return 127;
}
