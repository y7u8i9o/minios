/* diff: line differences between two files, computed with a longest
 * common subsequence table. Output uses < for the first file and > for
 * the second. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

static char **load(const char *path, int *count)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "diff: %s: %s\n", path, strerror(errno));
        exit(2);
    }
    char **lines = NULL;
    int n = 0, cap = 0;
    char line[1024];
    while (fgets(line, sizeof line, f)) {
        if (n == cap) {
            cap = cap ? cap * 2 : 64;
            lines = realloc(lines, (size_t)cap * sizeof *lines);
        }
        lines[n++] = strdup(line);
    }
    fclose(f);
    *count = n;
    return lines;
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: diff file1 file2\n");
        return 2;
    }
    int na, nb;
    char **a = load(argv[1], &na), **b = load(argv[2], &nb);
    /* lcs[i][j]: length of the LCS of a[i..] and b[j..]. */
    int w = nb + 1;
    int *lcs = calloc((size_t)(na + 1) * (size_t)w, sizeof *lcs);
    for (int i = na - 1; i >= 0; i--)
        for (int j = nb - 1; j >= 0; j--)
            lcs[i * w + j] = strcmp(a[i], b[j]) == 0 ? lcs[(i + 1) * w + j + 1] + 1
                             : (lcs[(i + 1) * w + j] > lcs[i * w + j + 1] ? lcs[(i + 1) * w + j]
                                                                          : lcs[i * w + j + 1]);
    int i = 0, j = 0, changed = 0;
    while (i < na || j < nb) {
        if (i < na && j < nb && strcmp(a[i], b[j]) == 0) {
            i++;
            j++;
        } else if (j < nb && (i >= na || lcs[(i + 1) * w + j] <= lcs[i * w + j + 1])) {
            printf("%da%d\n> %s", i, j + 1, b[j]);
            j++;
            changed = 1;
        } else {
            printf("%dd%d\n< %s", i + 1, j, a[i]);
            i++;
            changed = 1;
        }
    }
    return changed;
}
