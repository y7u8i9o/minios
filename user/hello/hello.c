#include <stdio.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    printf("hello from user mode, pid %d, ppid %d, argc %d\n", getpid(), getppid(), argc);
    for (int i = 0; i < argc; i++)
        printf("  argv[%d] = \"%s\"\n", i, argv[i]);
    return 7;
}
