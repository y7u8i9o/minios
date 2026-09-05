#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *field(char **position)
{
    char *p = *position;
    while (*p == ' ' || *p == '\t')
        p++;
    char *start = p;
    while (*p && *p != ' ' && *p != '\t' && *p != '\n')
        p++;
    if (*p)
        *p++ = 0;
    *position = p;
    return start;
}

int main(void)
{
    FILE *file = fopen("/dev/mounts", "r");
    if (!file) {
        perror("df: /dev/mounts");
        return 1;
    }
    printf("%-12s %12s %12s %12s %5s %s\n", "Filesystem", "1K-blocks", "Used", "Available", "Use%",
           "Mounted on");
    char *line = NULL;
    size_t capacity = 0;
    while (getline(&line, &capacity, file) >= 0) {
        char *p = line;
        char *path = field(&p), *type = field(&p);
        unsigned long total = strtoul(field(&p), NULL, 10);
        unsigned long free_blocks = strtoul(field(&p), NULL, 10);
        unsigned long block_size = strtoul(field(&p), NULL, 10);
        if (!block_size)
            continue;
        unsigned long used = total > free_blocks ? total - free_blocks : 0;
        unsigned long percent = total ? (unsigned long)((double)used * 100.0 / total + 0.5) : 0;
        printf("%-12s %12lu %12lu %12lu %4lu%% %s\n", type, total * block_size / 1024,
               used * block_size / 1024, free_blocks * block_size / 1024, percent, path);
    }
    int status = ferror(file);
    free(line);
    fclose(file);
    return status;
}
