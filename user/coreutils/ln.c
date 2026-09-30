/* ln: create hard links or, with -s, symbolic links.
 *
 *   ln [-sf] target [name]
 *   ln [-sf] target... directory
 *
 * A name that is an existing directory (or a symbolic link to one)
 * receives links named after the last component of each target; without
 * a name the link is created in the working directory. -f removes an
 * existing name first. A symbolic link stores the target text as given,
 * so a relative target is resolved from the directory holding the link.
 * A filesystem that cannot hold a hard link (FAT) or a symbolic link
 * (FAT, devfs) reports the kernel's error. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>

static int symbolic, force;

static const char *last_component(const char *path)
{
    size_t n = strlen(path);
    while (n > 1 && path[n - 1] == '/')
        n--;
    static char name[256];
    size_t end = n;
    while (n > 0 && path[n - 1] != '/')
        n--;
    size_t len = end - n;
    if (len >= sizeof name)
        len = sizeof name - 1;
    memcpy(name, path + n, len);
    name[len] = '\0';
    return name;
}

static int make_link(const char *target, const char *name)
{
    if (force) {
        struct stat st;
        if (lstat(name, &st) == 0 && !S_ISDIR(st.st_mode) && unlink(name) < 0) {
            fprintf(stderr, "ln: %s: %s\n", name, strerror(errno));
            return 1;
        }
    }
    if ((symbolic ? symlink(target, name) : link(target, name)) < 0) {
        fprintf(stderr, "ln: %s: %s\n", name, strerror(errno));
        return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    int first = 1;
    for (; first < argc && argv[first][0] == '-' && argv[first][1]; first++) {
        if (strcmp(argv[first], "--") == 0) {
            first++;
            break;
        }
        for (const char *p = argv[first] + 1; *p; p++) {
            if (*p == 's') {
                symbolic = 1;
            } else if (*p == 'f') {
                force = 1;
            } else {
                fprintf(stderr, "usage: ln [-sf] target [name]\n       ln [-sf] target... directory\n");
                return 2;
            }
        }
    }
    int count = argc - first;
    if (count < 1) {
        fprintf(stderr, "usage: ln [-sf] target [name]\n       ln [-sf] target... directory\n");
        return 2;
    }
    if (count == 1)
        return make_link(argv[first], last_component(argv[first]));
    const char *dest = argv[argc - 1];
    struct stat st;
    int into_dir = stat(dest, &st) == 0 && S_ISDIR(st.st_mode);
    if (count == 2 && !into_dir)
        return make_link(argv[first], dest);
    if (!into_dir) {
        fprintf(stderr, "ln: %s: %s\n", dest, strerror(ENOTDIR));
        return 1;
    }
    int status = 0;
    for (int i = first; i < argc - 1; i++) {
        char name[1024];
        int n = snprintf(name, sizeof name, "%s/%s", dest, last_component(argv[i]));
        if (n < 0 || (size_t)n >= sizeof name) {
            fprintf(stderr, "ln: %s: %s\n", argv[i], strerror(ENAMETOOLONG));
            status = 1;
            continue;
        }
        status |= make_link(argv[i], name);
    }
    return status;
}
