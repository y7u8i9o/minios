/* tail: bounded rings retain only the requested suffix of streamed input. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>
#include <time.h>

static size_t limit = 10;
static int bytes, follow;

static int suffix(FILE *file)
{
    if (!limit) {
        while (fgetc(file) != EOF)
            ;
        return ferror(file);
    }
    if (bytes) {
        unsigned char *ring = malloc(limit);
        if (!ring)
            return 1;
        size_t count = 0, head = 0;
        int c;
        while ((c = fgetc(file)) != EOF) {
            ring[head] = (unsigned char)c;
            head = (head + 1) % limit;
            if (count < limit)
                count++;
        }
        size_t start = count == limit ? head : 0;
        for (size_t i = 0; i < count; i++)
            putchar(ring[(start + i) % limit]);
        free(ring);
    } else {
        char **ring = calloc(limit, sizeof *ring);
        if (!ring)
            return 1;
        size_t count = 0, head = 0, capacity = 0;
        char *line = NULL;
        while (getline(&line, &capacity, file) >= 0) {
            free(ring[head]);
            ring[head] = strdup(line);
            if (!ring[head]) {
                for (size_t i = 0; i < limit; i++)
                    free(ring[i]);
                free(ring);
                free(line);
                return 1;
            }
            head = (head + 1) % limit;
            if (count < limit)
                count++;
        }
        size_t start = count == limit ? head : 0;
        for (size_t i = 0; i < count; i++)
            fputs(ring[(start + i) % limit], stdout);
        for (size_t i = 0; i < limit; i++)
            free(ring[i]);
        free(ring);
        free(line);
    }
    return ferror(file) || ferror(stdout);
}

int main(int argc, char **argv)
{
    int first = 1;
    for (; first < argc && argv[first][0] == '-' && argv[first][1]; first++) {
        if (!strcmp(argv[first], "--")) {
            first++;
            break;
        }
        if (!strcmp(argv[first], "-f")) {
            follow = 1;
            continue;
        }
        if (argv[first][1] != 'n' && argv[first][1] != 'c') {
            fprintf(stderr, "usage: tail [-f] [-n count | -c count] [file...]\n");
            return 2;
        }
        bytes = argv[first][1] == 'c';
        const char *value = argv[first] + 2;
        if (!*value && first + 1 < argc)
            value = argv[++first];
        char *end;
        long n = strtol(value, &end, 10);
        if (!*value || *end || n < 0 || n > 16777216) {
            fprintf(stderr, "tail: invalid count\n");
            return 2;
        }
        limit = (size_t)n;
    }
    int count = argc - first;
    if (!count)
        count = 1;
    FILE **files = calloc((size_t)count, sizeof *files);
    long *offsets = calloc((size_t)count, sizeof *offsets);
    if (!files || !offsets)
        return 1;
    int status = 0;
    for (int i = 0; i < count; i++) {
        const char *name = first + i < argc ? argv[first + i] : "-";
        files[i] = !strcmp(name, "-") ? stdin : fopen(name, "r");
        if (!files[i]) {
            fprintf(stderr, "tail: %s: %s\n", name, strerror(errno));
            status = 1;
            continue;
        }
        if (count > 1)
            printf("==> %s <==\n", name);
        status |= suffix(files[i]);
        offsets[i] = ftell(files[i]);
    }
    while (follow && !status) {
        fflush(stdout);
        struct timespec delay = {.tv_nsec = 200000000};
        nanosleep(&delay, NULL);
        for (int i = 0; i < count; i++) {
            struct stat st;
            if (fstat(fileno(files[i]), &st) < 0) {
                status = 1;
                break;
            }
            if (!S_ISREG(st.st_mode)) {
                follow = 0;
                continue;
            }
            if (st.st_size < offsets[i]) {
                fseek(files[i], 0, SEEK_SET);
                offsets[i] = 0;
            }
            if (st.st_size > offsets[i]) {
                clearerr(files[i]);
                int c;
                while ((c = fgetc(files[i])) != EOF)
                    putchar(c);
                offsets[i] = ftell(files[i]);
            }
        }
    }
    for (int i = 0; i < count; i++)
        if (files[i] && files[i] != stdin)
            fclose(files[i]);
    free(files);
    free(offsets);
    return status;
}
