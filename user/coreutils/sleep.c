/* sleep: pause for a number of seconds (fractions allowed: 0.5). */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: sleep seconds\n");
        return 1;
    }
    char *end;
    unsigned long ms = strtoul(argv[1], &end, 10) * 1000;
    if (*end == '.') {
        unsigned long scale = 100;
        for (end++; *end >= '0' && *end <= '9' && scale; end++, scale /= 10)
            ms += (unsigned long)(*end - '0') * scale;
    }
    sleep_ms(ms);
    return 0;
}
