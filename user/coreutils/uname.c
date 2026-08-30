/* uname: print system identification. -s kernel name, -n node name,
 * -r release, -v version, -m machine, -a everything. */
#include <stdio.h>
#include <string.h>
#include <sys/utsname.h>

int main(int argc, char **argv)
{
    int s = 0, n = 0, r = 0, v = 0, m = 0;
    if (argc == 1)
        s = 1;
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] != '-') {
            fprintf(stderr, "usage: uname [-asnrvm]\n");
            return 2;
        }
        for (const char *o = argv[i] + 1; *o; o++) {
            switch (*o) {
            case 'a': s = n = r = v = m = 1; break;
            case 's': s = 1; break;
            case 'n': n = 1; break;
            case 'r': r = 1; break;
            case 'v': v = 1; break;
            case 'm': m = 1; break;
            default:
                fprintf(stderr, "uname: unknown option -%c\n", *o);
                return 2;
            }
        }
    }
    struct utsname u;
    if (uname(&u) < 0) {
        perror("uname");
        return 1;
    }
    const char *parts[5];
    int k = 0;
    if (s) parts[k++] = u.sysname;
    if (n) parts[k++] = u.nodename;
    if (r) parts[k++] = u.release;
    if (v) parts[k++] = u.version;
    if (m) parts[k++] = u.machine;
    for (int i = 0; i < k; i++)
        printf("%s%s", i ? " " : "", parts[i]);
    printf("\n");
    return 0;
}
