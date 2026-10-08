#pragma once
/* A hash map from int keys to int values with open addressing, for the
 * row maps of the data views. INT_MIN cannot be a key. */
struct intmap {
    int *keys, *vals;
    int cap, n;                 /* cap is 0 or a power of two */
};

/* Stores value under key. Returns 0, or -1 without memory. */
int intmap_put(struct intmap *m, int key, int value);
/* The value under key, or fallback without the key. */
int intmap_get(const struct intmap *m, int key, int fallback);
void intmap_remove(struct intmap *m, int key);
void intmap_clear(struct intmap *m);
void intmap_free(struct intmap *m);
