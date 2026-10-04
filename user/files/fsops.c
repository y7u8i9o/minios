/* File operations of the Files program: path helpers and sizes. Copy,
 * remove and move are shared with the file chooser and the desktop
 * (gui/fileops.h). Errors are -errno. */
#include "files.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <gui/i18n.h>

void fs_join(char *out, size_t size, const char *dir, const char *name)
{
    if (strcmp(dir, "/") == 0)
        snprintf(out, size, "/%s", name);
    else
        snprintf(out, size, "%s/%s", dir, name);
}

void fs_normalize(char *path)
{
    char *parts[64];
    int n = 0;
    char copy[512];
    strlcpy(copy, path, sizeof copy);
    for (char *p = strtok(copy, "/"); p; p = strtok(NULL, "/")) {
        if (strcmp(p, ".") == 0 || !p[0])
            continue;
        if (strcmp(p, "..") == 0) {
            if (n > 0)
                n--;
            continue;
        }
        if (n < 64)
            parts[n++] = p;
    }
    size_t len = 0;
    path[0] = '\0';
    for (int i = 0; i < n; i++) {
        len += (size_t)snprintf(path + len, 512 - len, "/%s", parts[i]);
        if (len >= 511)
            break;
    }
    if (n == 0)
        strcpy(path, "/");
}

const char *fs_basename(const char *path)
{
    const char *slash = strrchr(path, '/');
    if (!slash || !slash[1])
        return path[0] == '/' && !path[1] ? "/" : path;
    return slash + 1;
}

long fs_tree_size(const char *path, int *files, int *dirs)
{
    struct stat st;
    if (lstat(path, &st) < 0)
        return 0;
    if (!S_ISDIR(st.st_mode)) {
        (*files)++;
        return (long)st.st_size;
    }
    (*dirs)++;
    long total = 0;
    DIR *d = opendir(path);
    if (!d)
        return 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        char child[512];
        fs_join(child, sizeof child, path, e->d_name);
        total += fs_tree_size(child, files, dirs);
    }
    closedir(d);
    return total;
}
