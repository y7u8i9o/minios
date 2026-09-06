#include <libgen.h>
#include <string.h>

/* Return the last component of path. A path that is empty or consists of
 * slashes yields "." or "/". Trailing slashes are removed from the argument. */
char *basename(char *path)
{
    static char dot[] = ".";
    if (path == NULL || *path == '\0')
        return dot;
    size_t n = strlen(path);
    while (n > 1 && path[n - 1] == '/')
        path[--n] = '\0';
    if (n == 1 && path[0] == '/')
        return path;
    char *slash = strrchr(path, '/');
    return slash != NULL ? slash + 1 : path;
}

/* Return the directory part of path, "." when there is none. */
char *dirname(char *path)
{
    static char dot[] = ".";
    static char root[] = "/";
    if (path == NULL || *path == '\0')
        return dot;
    size_t n = strlen(path);
    while (n > 1 && path[n - 1] == '/')
        path[--n] = '\0';
    char *slash = strrchr(path, '/');
    if (slash == NULL)
        return dot;
    while (slash > path && *slash == '/')
        slash--;
    if (slash == path && *slash == '/')
        return root;
    slash[1] = '\0';
    return path;
}
