/* The int hash map of intmap.h. A removal moves the following entries of
 * the probe sequence back, so the map needs no deleted markers. */
#include "intmap.h"
#include <limits.h>
#include <stdlib.h>

#define EMPTY INT_MIN

static unsigned slot_of(const struct intmap *m, int key)
{
    return ((unsigned)key * 2654435761u) & (unsigned)(m->cap - 1);
}

static int grow(struct intmap *m)
{
    int cap = m->cap ? 2 * m->cap : 16;
    int *keys = malloc((size_t)cap * sizeof *keys), *vals = malloc((size_t)cap * sizeof *vals);
    if (!keys || !vals) {
        free(keys);
        free(vals);
        return -1;
    }
    for (int i = 0; i < cap; i++)
        keys[i] = EMPTY;
    struct intmap old = *m;
    m->keys = keys;
    m->vals = vals;
    m->cap = cap;
    m->n = 0;
    for (int i = 0; i < old.cap; i++)
        if (old.keys[i] != EMPTY)
            intmap_put(m, old.keys[i], old.vals[i]);
    free(old.keys);
    free(old.vals);
    return 0;
}

int intmap_put(struct intmap *m, int key, int value)
{
    if ((m->n + 1) * 4 > m->cap * 3 && grow(m) < 0)
        return -1;
    unsigned i = slot_of(m, key);
    while (m->keys[i] != EMPTY && m->keys[i] != key)
        i = (i + 1) & (unsigned)(m->cap - 1);
    if (m->keys[i] == EMPTY)
        m->n++;
    m->keys[i] = key;
    m->vals[i] = value;
    return 0;
}

int intmap_get(const struct intmap *m, int key, int fallback)
{
    if (!m->cap)
        return fallback;
    for (unsigned i = slot_of(m, key); m->keys[i] != EMPTY; i = (i + 1) & (unsigned)(m->cap - 1))
        if (m->keys[i] == key)
            return m->vals[i];
    return fallback;
}

void intmap_remove(struct intmap *m, int key)
{
    if (!m->cap)
        return;
    unsigned mask = (unsigned)(m->cap - 1), i = slot_of(m, key);
    while (m->keys[i] != key) {
        if (m->keys[i] == EMPTY)
            return;
        i = (i + 1) & mask;
    }
    m->keys[i] = EMPTY;
    m->n--;
    /* Entries after the hole whose home slot does not lie between the
     * hole and their slot move into the hole. */
    for (unsigned j = (i + 1) & mask; m->keys[j] != EMPTY; j = (j + 1) & mask) {
        unsigned home = slot_of(m, m->keys[j]);
        if ((j > i && (home <= i || home > j)) || (j < i && home <= i && home > j)) {
            m->keys[i] = m->keys[j];
            m->vals[i] = m->vals[j];
            m->keys[j] = EMPTY;
            i = j;
        }
    }
}

void intmap_clear(struct intmap *m)
{
    for (int i = 0; i < m->cap; i++)
        m->keys[i] = EMPTY;
    m->n = 0;
}

void intmap_free(struct intmap *m)
{
    free(m->keys);
    free(m->vals);
    *m = (struct intmap){ 0 };
}
