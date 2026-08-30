/* yes: repeat a line forever. */
#include <stdio.h>

int main(int argc, char **argv)
{
    const char *s = argc > 1 ? argv[1] : "y";
    for (;;) {
        if (puts(s) < 0)
            return 1;
    }
}
