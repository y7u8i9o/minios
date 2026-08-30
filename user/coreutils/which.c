/* which: locate a program in PATH. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

int main(int argc, char **argv)
{
    const char *path = getenv("PATH");
    if (!path)
        path = "/bin";
    int status = 0;
    for (int i = 1; i < argc; i++) {
        int found = 0;
        if (strchr(argv[i], '/')) {
            struct stat st;
            found = stat(argv[i], &st) == 0;
            if (found)
                puts(argv[i]);
        } else {
            const char *p = path;
            while (*p && !found) {
                const char *end = strchr(p, ':');
                size_t n = end ? (size_t)(end - p) : strlen(p);
                char full[512];
                snprintf(full, sizeof full, "%.*s/%s", (int)n, p, argv[i]);
                struct stat st;
                if (stat(full, &st) == 0 && S_ISREG(st.st_mode)) {
                    puts(full);
                    found = 1;
                }
                p += n + (end ? 1 : 0);
            }
        }
        if (!found)
            status = 1;
    }
    return status;
}
