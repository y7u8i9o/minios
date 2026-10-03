/* chmod: change the permission bits of files (chmod(1)).
 *
 *     chmod [-R] mode file...
 *
 * mode is an octal number or a comma separated list of symbolic clauses
 * [ugoa]*[+-=][rwxXst]*, as in POSIX. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>

static const char *mode_text;
static int status;

/* Apply the mode text to the old mode of a file. Returns -1 for a
 * malformed mode. */
static int apply_mode(const char *text, mode_t old, int is_dir, mode_t *out)
{
    if (text[0] >= '0' && text[0] <= '7') {
        char *end;
        unsigned long v = strtoul(text, &end, 8);
        if (*end || v > 07777)
            return -1;
        *out = (mode_t)v;
        return 0;
    }
    mode_t mode = old & 07777;
    mode_t mask = umask(0);
    umask(mask);
    const char *p = text;
    for (;;) {
        mode_t who = 0;
        for (; *p && strchr("ugoa", *p); p++)
            who |= *p == 'u' ? 04700 : *p == 'g' ? 02070 : *p == 'o' ? 01007 : 07777;
        int explicit_who = who != 0;
        if (!explicit_who)
            who = 07777;
        if (!*p || !strchr("+-=", *p))
            return -1;
        while (*p && strchr("+-=", *p)) {
            char op = *p++;
            mode_t bits = 0;
            for (; *p && strchr("rwxXst", *p); p++) {
                switch (*p) {
                case 'r': bits |= 0444; break;
                case 'w': bits |= 0222; break;
                case 'x': bits |= 0111; break;
                case 'X':
                    if (is_dir || (old & 0111))
                        bits |= 0111;
                    break;
                case 's': bits |= S_ISUID | S_ISGID; break;
                case 't': bits |= S_ISVTX; break;
                }
            }
            bits &= who;
            /* Without u, g, o or a the umask limits what + and = grant. */
            if (!explicit_who && op != '-')
                bits &= ~mask;
            if (op == '+')
                mode |= bits;
            else if (op == '-')
                mode &= ~bits;
            else
                mode = (mode & ~who) | bits;
        }
        if (*p == '\0')
            break;
        if (*p++ != ',')
            return -1;
    }
    *out = mode;
    return 0;
}

static void change(const char *path, int recursive)
{
    struct stat st;
    if (stat(path, &st) < 0) {
        fprintf(stderr, "chmod: %s: %s\n", path, strerror(errno));
        status = 1;
        return;
    }
    mode_t mode;
    if (apply_mode(mode_text, st.st_mode, S_ISDIR(st.st_mode), &mode) < 0) {
        fprintf(stderr, "chmod: invalid mode: %s\n", mode_text);
        exit(1);
    }
    if (chmod(path, mode) < 0) {
        fprintf(stderr, "chmod: %s: %s\n", path, strerror(errno));
        status = 1;
    }
    if (!recursive || !S_ISDIR(st.st_mode))
        return;
    DIR *d = opendir(path);
    if (!d) {
        fprintf(stderr, "chmod: %s: %s\n", path, strerror(errno));
        status = 1;
        return;
    }
    struct dirent *e;
    while ((e = readdir(d))) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        char sub[1024];
        snprintf(sub, sizeof sub, "%s/%s", path, e->d_name);
        struct stat lst;
        /* Symbolic links met during the walk are not followed. */
        if (lstat(sub, &lst) == 0 && S_ISLNK(lst.st_mode))
            continue;
        change(sub, 1);
    }
    closedir(d);
}

int main(int argc, char **argv)
{
    int recursive = 0, i = 1;
    if (i < argc && strcmp(argv[i], "-R") == 0) {
        recursive = 1;
        i++;
    }
    if (argc - i < 2) {
        fprintf(stderr, "usage: chmod [-R] mode file...\n");
        return 2;
    }
    mode_text = argv[i++];
    mode_t check;
    if (apply_mode(mode_text, 0, 0, &check) < 0) {
        fprintf(stderr, "chmod: invalid mode: %s\n", mode_text);
        return 1;
    }
    for (; i < argc; i++)
        change(argv[i], recursive);
    return status;
}
