/* tr: translate or delete characters. Sets may contain ranges such as
 * a-z. -d deletes the characters of SET1, -s squeezes repeats of the
 * characters of the last set into one. */
#include <stdio.h>
#include <string.h>

/* Decode \n, \t, \r, \\ and \NNN (octal) in a set. */
static int unescape(const char *set, unsigned char *out)
{
    int n = 0;
    for (size_t i = 0; set[i] && n < 256; i++) {
        if (set[i] != '\\' || !set[i + 1]) {
            out[n++] = (unsigned char)set[i];
            continue;
        }
        i++;
        switch (set[i]) {
        case 'n': out[n++] = '\n'; break;
        case 't': out[n++] = '\t'; break;
        case 'r': out[n++] = '\r'; break;
        case '\\': out[n++] = '\\'; break;
        default:
            if (set[i] >= '0' && set[i] <= '7') {
                int v = 0, k = 0;
                while (k < 3 && set[i] >= '0' && set[i] <= '7') {
                    v = v * 8 + (set[i] - '0');
                    i++;
                    k++;
                }
                i--;
                out[n++] = (unsigned char)v;
            } else {
                out[n++] = (unsigned char)set[i];
            }
        }
    }
    return n;
}

static int expand(const char *set, unsigned char *out)
{
    unsigned char raw[257];
    int rn = unescape(set, raw);
    raw[rn] = '\0';
    int n = 0;
    for (int i = 0; i < rn && n < 256; i++) {
        if (i + 2 < rn && raw[i + 1] == '-') {
            for (int c = raw[i]; c <= raw[i + 2] && n < 256; c++)
                out[n++] = (unsigned char)c;
            i += 2;
        } else {
            out[n++] = raw[i];
        }
    }
    return n;
}

int main(int argc, char **argv)
{
    int del = 0, squeeze = 0, a = 1;
    while (a < argc && argv[a][0] == '-' && argv[a][1]) {
        for (const char *o = argv[a] + 1; *o; o++) {
            if (*o == 'd') del = 1;
            else if (*o == 's') squeeze = 1;
            else {
                fprintf(stderr, "usage: tr [-ds] SET1 [SET2]\n");
                return 2;
            }
        }
        a++;
    }
    if (argc <= a || (!del && !squeeze && argc <= a + 1)) {
        fprintf(stderr, "usage: tr [-ds] SET1 [SET2]\n");
        return 2;
    }
    unsigned char s1[256], s2[256];
    int n1 = expand(argv[a], s1);
    int n2 = (del || argc <= a + 1) ? 0 : expand(argv[a + 1], s2);
    int map[256], squeezed[256] = { 0 };
    for (int c = 0; c < 256; c++)
        map[c] = c;
    for (int i = 0; i < n1; i++)
        map[s1[i]] = del ? -1 : (n2 ? s2[i < n2 ? i : n2 - 1] : s1[i]);
    if (squeeze) {
        if (n2)
            for (int i = 0; i < n2; i++) squeezed[s2[i]] = 1;
        else
            for (int i = 0; i < n1; i++) squeezed[s1[i]] = 1;
    }
    int c, last = -1;
    while ((c = getchar()) != EOF) {
        int m = map[c];
        if (m < 0)
            continue;
        if (squeezed[m] && m == last)
            continue;
        putchar(m);
        last = m;
    }
    return 0;
}
