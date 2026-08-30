/* printf: format arguments. Supports %s %d %i %u %x %o %c %% with width
 * and precision, and the escapes \n \t \\ \a \b \r \0NNN in the format. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: printf format [args...]\n");
        return 2;
    }
    const char *f = argv[1];
    int ai = 2;
    for (const char *p = f; *p; p++) {
        if (*p == '\\') {
            p++;
            switch (*p) {
            case 'n': putchar('\n'); break;
            case 't': putchar('\t'); break;
            case 'r': putchar('\r'); break;
            case 'a': putchar(7); break;
            case 'b': putchar('\b'); break;
            case '\\': putchar('\\'); break;
            case '0': {
                int v = 0, n = 0;
                while (n < 3 && p[1] >= '0' && p[1] <= '7') {
                    v = v * 8 + (p[1] - '0');
                    p++;
                    n++;
                }
                putchar(v);
                break;
            }
            case '\0': putchar('\\'); p--; break;
            default: putchar('\\'); putchar(*p); break;
            }
            continue;
        }
        if (*p != '%') {
            putchar(*p);
            continue;
        }
        char spec[32];
        size_t n = 0;
        spec[n++] = '%';
        p++;
        while (*p && strchr("-+ 0123456789.", *p) && n < sizeof spec - 3)
            spec[n++] = *p++;
        char conv = *p;
        if (conv == '%') {
            putchar('%');
            continue;
        }
        const char *arg = ai < argc ? argv[ai++] : "";
        switch (conv) {
        case 's':
            spec[n++] = 's'; spec[n] = '\0';
            printf(spec, arg);
            break;
        case 'c':
            spec[n++] = 'c'; spec[n] = '\0';
            printf(spec, arg[0]);
            break;
        case 'd': case 'i':
            spec[n++] = 'l'; spec[n++] = 'd'; spec[n] = '\0';
            printf(spec, strtol(arg, NULL, 0));
            break;
        case 'u': case 'x': case 'X': case 'o':
            spec[n++] = 'l'; spec[n++] = conv; spec[n] = '\0';
            printf(spec, strtoul(arg, NULL, 0));
            break;
        default:
            fprintf(stderr, "printf: unknown conversion %%%c\n", conv);
            return 2;
        }
        if (!conv)
            break;
    }
    return 0;
}
