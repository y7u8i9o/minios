#pragma once
/* Data models behind tree views and tables. Rows are integers chosen by
 * the model (the root is -1); views ask for children on demand, so
 * large data is never copied. */
#include <stddef.h>

struct image;

struct model {
    int (*rows)(struct model *m, int parent);               /* number of children of parent */
    int (*child)(struct model *m, int parent, int index);   /* row id of the index-th child */
    int (*columns)(struct model *m);
    /* Cell text into buf; may return a static string instead. */
    const char *(*cell)(struct model *m, int row, int col, char *buf, size_t size);
    const char *(*header)(struct model *m, int col);        /* may be NULL */
    void (*sort)(struct model *m, int col, int descending); /* may be NULL */
    void *user;
    /* Icon drawn before the first column of a row; may be NULL. */
    const struct image *(*icon)(struct model *m, int row);
};
