/* The pinyin core of imed (I3, docs/design/ime.md).
 *
 * The letters become a lattice of syllable edges.  An edge is a whole
 * syllable, an incomplete syllable at the end of the letters (zhon), or
 * an abbreviation by the initial (z or zh for any syllable that begins
 * with it), where the next letter cannot continue a syllable (the z of
 * zg, not the z of zhong).  An apostrophe ends a syllable.  Because the
 * syllables of the dictionary are sorted, each edge stands for a range of
 * syllable numbers.  The words of the dictionary and of the user
 * dictionary whose syllables fall in the ranges of a path of edges become
 * word edges, scored by the logarithm of their probability, the weight
 * divided by the sum of all weights.  The division of
 * the letters into the fewest and most complete syllables gives the
 * auxiliary line.  A Viterbi search over the word edges that follow this
 * division gives the sentence, and the word edges at the start give the
 * other candidates.
 *
 * The state below belongs to the single thread of imed. */
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pycore.h"

#define MAX_SEDGES 16
#define MAX_WEDGES 8192
#define MAX_USER 4096
#define WORD_PENALTY 0.5        /* per word of a sentence, besides its probability */
#define ABBR_PENALTY 3.0        /* per abbreviated syllable */
#define PART_PENALTY 1.0        /* per incomplete syllable */

enum { E_FULL, E_PART, E_ABBR };

struct sedge {
    int to;
    uint16_t lo, hi;
    int kind;
};

struct wedge {
    int from, to;
    int on_division;            /* its syllables follow the division and none is abbreviated */
    double score;
    char text[96];
    uint16_t ids[PY_MAX_WORD_SYLLABLES];
    int nids;
};

struct user_word {
    char word[96];
    uint16_t ids[PY_MAX_WORD_SYLLABLES];
    int nids, count;
};

static unsigned char *data;
static uint32_t nsyl, nentries;
static const char (*syl)[8];
static const unsigned char *entries;
static const char *pool;
static uint32_t pool_size;
static double log_total;                        /* the logarithm of the sum of the weights */

static struct user_word users[MAX_USER];
static int nusers;
static char user_path[256];

/* The lattice of the current letters. */
static char clean[PY_MAX_LETTERS + 1];
static int n, boundary[PY_MAX_LETTERS + 1];
static struct sedge sedges[PY_MAX_LETTERS][MAX_SEDGES];
static int nsedges[PY_MAX_LETTERS];
static struct wedge wedges[MAX_WEDGES];
static int nwedges;
static int division[PY_MAX_LETTERS + 1];        /* 1 at the start of each syllable of the division */

/* ---- the dictionary ---- */

static uint32_t u32(const unsigned char *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint16_t entry_id(uint32_t e, int k)
{
    const unsigned char *p = entries + (size_t)e * 16 + 2 * k;
    return (uint16_t)(p[0] | p[1] << 8);
}

static int entry_len(uint32_t e)
{
    int k = 0;
    while (k < 4 && entry_id(e, k) != 0xffff)
        k++;
    return k;
}

static uint32_t entry_weight(uint32_t e) { return u32(entries + (size_t)e * 16 + 8); }
static const char *entry_word(uint32_t e) { return pool + u32(entries + (size_t)e * 16 + 12); }

int py_load(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return -errno;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    data = size > 28 ? malloc((size_t)size) : NULL;
    if (!data || fread(data, 1, (size_t)size, f) != (size_t)size || memcmp(data, "MPY1", 4) != 0) {
        fclose(f);
        free(data);
        data = NULL;
        return -EINVAL;
    }
    fclose(f);
    nsyl = u32(data + 4);
    nentries = u32(data + 8);
    uint32_t syl_off = u32(data + 12), entry_off = u32(data + 16), pool_off = u32(data + 20);
    pool_size = u32(data + 24);
    if ((size_t)pool_off + pool_size > (size_t)size || (size_t)entry_off + (size_t)nentries * 16 > (size_t)size ||
        (size_t)syl_off + (size_t)nsyl * 8 > (size_t)size || nsyl > 0xfff0) {
        free(data);
        data = NULL;
        return -EINVAL;
    }
    syl = (const char (*)[8])(data + syl_off);
    entries = data + entry_off;
    pool = (const char *)data + pool_off;
    double total = 0;
    for (uint32_t e = 0; e < nentries; e++)
        total += 1.0 + entry_weight(e);
    log_total = log(total);
    return 0;
}

const char *py_syllable(int id)
{
    static char name[9];
    if (id < 0 || (uint32_t)id >= nsyl)
        return "";
    memcpy(name, syl[id], 8);
    name[8] = '\0';
    return name;
}

int py_syllable_count(void)
{
    return (int)nsyl;
}

/* syl_range finds the syllables that begin with s[0..len): 1 with their
 * numbers in lo..hi, 0 without one. */
static int syl_range(const char *s, int len, int *lo, int *hi)
{
    int a = 0, b = (int)nsyl;
    while (a < b) {
        int m = (a + b) / 2;
        if (strncmp(syl[m], s, (size_t)len) < 0)
            a = m + 1;
        else
            b = m;
    }
    if (a >= (int)nsyl || strncmp(syl[a], s, (size_t)len) != 0)
        return 0;
    int last = a;
    while (last + 1 < (int)nsyl && strncmp(syl[last + 1], s, (size_t)len) == 0)
        last++;
    *lo = a;
    *hi = last;
    return 1;
}

static int syl_exact(const char *s, int len)
{
    int lo, hi;
    if (len > 7 || !syl_range(s, len, &lo, &hi))
        return -1;
    return syl[lo][len] == '\0' ? lo : -1;
}

/* first_entry finds the first entry whose first syllable is at least id. */
static uint32_t first_entry(int id)
{
    uint32_t a = 0, b = nentries;
    while (a < b) {
        uint32_t m = (a + b) / 2;
        if (entry_id(m, 0) < id)
            a = m + 1;
        else
            b = m;
    }
    return a;
}

/* ---- the user dictionary ---- */

static struct user_word *user_find(const char *word, const uint16_t *ids, int nids)
{
    for (int i = 0; i < nusers; i++)
        if (users[i].nids == nids && memcmp(users[i].ids, ids, (size_t)nids * sizeof ids[0]) == 0 &&
            strcmp(users[i].word, word) == 0)
            return &users[i];
    return NULL;
}

/* ids_of reads syllable names separated by spaces. */
static int ids_of(char *names, uint16_t *ids)
{
    int k = 0;
    for (char *s = strtok(names, " "); s && k < PY_MAX_WORD_SYLLABLES; s = strtok(NULL, " ")) {
        int id = syl_exact(s, (int)strlen(s));
        if (id < 0)
            return 0;
        ids[k++] = (uint16_t)id;
    }
    return k;
}

/* The file has one line "word TAB syllables TAB count" for each choice.
 * A later line of the same word replaces the count of an earlier one. */
int py_user_load(const char *path)
{
    strlcpy(user_path, path, sizeof user_path);
    nusers = 0;
    FILE *f = fopen(path, "r");
    if (!f)
        return errno == ENOENT ? 0 : -errno;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\n")] = '\0';
        char *names = strchr(line, '\t'), *count = names ? strchr(names + 1, '\t') : NULL;
        if (!count)
            continue;
        *names++ = '\0';
        *count++ = '\0';
        uint16_t ids[PY_MAX_WORD_SYLLABLES];
        int k = ids_of(names, ids);
        if (!k || !line[0])
            continue;
        struct user_word *u = user_find(line, ids, k);
        if (!u && nusers < MAX_USER) {
            u = &users[nusers++];
            strlcpy(u->word, line, sizeof u->word);
            memcpy(u->ids, ids, (size_t)k * sizeof ids[0]);
            u->nids = k;
        }
        if (u)
            u->count = atoi(count);
    }
    fclose(f);
    return 0;
}

void py_user_learn(const char *word, const uint16_t *ids, int nids)
{
    if (nids < 1 || nids > PY_MAX_WORD_SYLLABLES || !word[0])
        return;
    struct user_word *u = user_find(word, ids, nids);
    if (!u) {
        if (nusers == MAX_USER)
            return;
        u = &users[nusers++];
        strlcpy(u->word, word, sizeof u->word);
        memcpy(u->ids, ids, (size_t)nids * sizeof ids[0]);
        u->nids = nids;
        u->count = 0;
    }
    u->count++;
    if (!user_path[0])
        return;
    FILE *f = fopen(user_path, "a");
    if (!f)
        return;
    fprintf(f, "%s\t", word);
    for (int i = 0; i < nids; i++)
        fprintf(f, "%s%s", i ? " " : "", py_syllable(ids[i]));
    fprintf(f, "\t%d\n", u->count);
    fclose(f);
}

static double user_boost(int count)
{
    return 4.0 + 2.0 * log(1.0 + count);
}

/* ---- the lattice ---- */

static int crosses_boundary(int from, int to)
{
    for (int k = from + 1; k < to; k++)
        if (boundary[k])
            return 1;
    return 0;
}

static void add_sedge(int p, int to, int lo, int hi, int kind)
{
    if (nsedges[p] < MAX_SEDGES)
        sedges[p][nsedges[p]++] = (struct sedge){ to, (uint16_t)lo, (uint16_t)hi, kind };
}

static void build_sedges(void)
{
    for (int p = 0; p < n; p++) {
        nsedges[p] = 0;
        for (int len = 1; len <= 6 && p + len <= n; len++) {
            if (crosses_boundary(p, p + len))
                break;
            int id = syl_exact(clean + p, len), lo, hi;
            if (id >= 0)
                add_sedge(p, p + len, id, id, E_FULL);
            else if (p + len == n && syl_range(clean + p, len, &lo, &hi))
                add_sedge(p, p + len, lo, hi, E_PART);
        }
        int lo, hi, x, y;
        if (strchr("bpmfdtnlgkhjqxrzcsyw", clean[p]) && p + 1 < n &&
            (boundary[p + 1] || !syl_range(clean + p, 2, &x, &y)) && syl_range(clean + p, 1, &lo, &hi))
            add_sedge(p, p + 1, lo, hi, E_ABBR);
        if (strchr("zcs", clean[p]) && p + 2 < n && clean[p + 1] == 'h' && !boundary[p + 1] &&
            (boundary[p + 2] || !syl_range(clean + p, 3, &x, &y)) && syl_range(clean + p, 2, &lo, &hi))
            add_sedge(p, p + 2, lo, hi, E_ABBR);
    }
}

static void add_wedge(int from, int to, int on_division, const char *text, const uint16_t *ids, int nids,
                      double score)
{
    for (int i = 0; i < nwedges; i++)
        if (wedges[i].from == from && wedges[i].to == to && strcmp(wedges[i].text, text) == 0) {
            if (score > wedges[i].score) {
                wedges[i].score = score;
                memcpy(wedges[i].ids, ids, (size_t)nids * sizeof ids[0]);
                wedges[i].nids = nids;
            }
            wedges[i].on_division |= on_division;
            return;
        }
    if (nwedges == MAX_WEDGES)
        return;
    struct wedge *w = &wedges[nwedges++];
    w->from = from;
    w->to = to;
    w->on_division = on_division;
    w->score = score;
    strlcpy(w->text, text, sizeof w->text);
    memcpy(w->ids, ids, (size_t)nids * sizeof ids[0]);
    w->nids = nids;
}

/* The syllables without a vowel (n, m, ng, hm, hng) are interjections,
 * which a division takes only when nothing else fits: nh is an
 * abbreviation of ni hao. */
static int interjection(int id)
{
    return !strpbrk(syl[id], "aeiouv");
}

static int in_ranges(const uint16_t *ids, const struct sedge *path, int k)
{
    for (int t = 0; t < k; t++)
        if (ids[t] < path[t].lo || ids[t] > path[t].hi)
            return 0;
    return 1;
}

/* match adds the words whose syllables fall in the ranges of path and
 * returns 1 when a longer word begins with them. */
static int match(int start, int end, const struct sedge *path, int k)
{
    double penalty = 0;
    int on_division = division[start];
    for (int t = 0; t < k; t++) {
        penalty += path[t].kind == E_ABBR ? ABBR_PENALTY : path[t].kind == E_PART ? PART_PENALTY : 0;
        on_division &= path[t].kind != E_ABBR && division[path[t].to] &&
                       !(path[t].lo == path[t].hi && interjection(path[t].lo));
    }
    int longer = 0;
    if (k <= 4) {
        for (uint32_t e = first_entry(path[0].lo); e < nentries && entry_id(e, 0) <= path[0].hi; e++) {
            int len = entry_len(e);
            if (len < k)
                continue;
            uint16_t ids[4];
            for (int t = 0; t < len; t++)
                ids[t] = entry_id(e, t);
            if (!in_ranges(ids, path, k))
                continue;
            if (len > k) {
                longer = 1;
                continue;
            }
            double score = log(1.0 + entry_weight(e)) - log_total - penalty;
            struct user_word *u = user_find(entry_word(e), ids, len);
            if (u)
                score += user_boost(u->count);
            add_wedge(start, end, on_division, entry_word(e), ids, len, score);
        }
    }
    for (int i = 0; i < nusers; i++) {
        struct user_word *u = &users[i];
        if (u->nids < k || !in_ranges(u->ids, path, k))
            continue;
        if (u->nids > k)
            longer = 1;
        else
            add_wedge(start, end, on_division, u->word, u->ids, u->nids,
                      log(1000.0) - log_total + user_boost(u->count) - penalty);
    }
    return longer;
}

static void dfs(int start, int pos, struct sedge *path, int k)
{
    for (int i = 0; i < nsedges[pos]; i++) {
        path[k] = sedges[pos][i];
        int longer = match(start, path[k].to, path, k + 1);
        if (longer && k + 1 < PY_MAX_WORD_SYLLABLES && path[k].to < n)
            dfs(start, path[k].to, path, k + 1);
    }
}

/* segment writes the letters divided into syllables: the division with the
 * fewest and most complete syllables. */
static void segment(char *aux, size_t size)
{
    double cost[PY_MAX_LETTERS + 1];
    int next[PY_MAX_LETTERS + 1];
    cost[n] = 0;
    for (int p = n - 1; p >= 0; p--) {
        cost[p] = 5.0 + cost[p + 1];        /* a letter outside every syllable */
        next[p] = p + 1;
        for (int i = 0; i < nsedges[p]; i++) {
            struct sedge *e = &sedges[p][i];
            double c = (e->kind == E_ABBR ? 2.5 : e->kind == E_PART ? 1.2 : interjection(e->lo) ? 3.0 : 1.0) +
                       cost[e->to];
            if (c < cost[p]) {
                cost[p] = c;
                next[p] = e->to;
            }
        }
    }
    memset(division, 0, sizeof division);
    for (int p = 0; p < n; p = next[p])
        division[p] = 1;
    division[n] = 1;
    size_t o = 0;
    for (int p = 0; p < n && aux && o + 2 < size; p = next[p]) {
        if (p > 0)
            aux[o++] = '\'';
        for (int q = p; q < next[p] && o + 1 < size; q++)
            aux[o++] = clean[q];
    }
    if (aux && size)
        aux[o] = '\0';
}

static int by_length_then_score(const void *a, const void *b)
{
    const struct wedge *x = *(struct wedge *const *)a, *y = *(struct wedge *const *)b;
    if (x->to != y->to)
        return y->to - x->to;
    return x->score < y->score ? 1 : x->score > y->score ? -1 : 0;
}

int py_candidates(const char *letters, struct py_cand *out, int max, char *aux, size_t auxsize)
{
    int rawpos[PY_MAX_LETTERS + 1], rawlen = 0;
    n = 0;
    memset(boundary, 0, sizeof boundary);
    for (const char *p = letters; *p && n < PY_MAX_LETTERS; p++, rawlen++) {
        if (*p == '\'') {
            if (n > 0)
                boundary[n] = 1;
            continue;
        }
        if (*p < 'a' || *p > 'z')
            continue;
        rawpos[n] = rawlen;
        clean[n++] = *p;
    }
    clean[n] = '\0';
    rawpos[n] = rawlen;
    if (aux && auxsize)
        aux[0] = '\0';
    if (!n || !data)
        return 0;
    build_sedges();
    segment(aux, auxsize);
    nwedges = 0;
    struct sedge path[PY_MAX_WORD_SYLLABLES];
    for (int s = 0; s < n; s++)
        dfs(s, s, path, 0);

    int count = 0;
    /* The sentence over all letters. */
    double best[PY_MAX_LETTERS + 1];
    int via[PY_MAX_LETTERS + 1];
    for (int p = 0; p <= n; p++) {
        best[p] = -1e30;
        via[p] = -1;
    }
    best[0] = 0;
    for (int p = 0; p < n; p++) {
        if (best[p] <= -1e29)
            continue;
        for (int i = 0; i < nwedges; i++)
            if (wedges[i].from == p && wedges[i].on_division) {
                double s = best[p] + wedges[i].score - WORD_PENALTY;
                if (s > best[wedges[i].to]) {
                    best[wedges[i].to] = s;
                    via[wedges[i].to] = i;
                }
            }
    }
    if (via[n] >= 0 && wedges[via[n]].from > 0 && count < max) {
        int chain[PY_MAX_LETTERS], nchain = 0;
        for (int p = n; p > 0 && nchain < PY_MAX_LETTERS; p = wedges[via[p]].from)
            chain[nchain++] = via[p];
        struct py_cand *c = &out[count];
        c->text[0] = '\0';
        c->nids = 0;
        for (int i = nchain - 1; i >= 0; i--) {
            struct wedge *w = &wedges[chain[i]];
            strlcat(c->text, w->text, sizeof c->text);
            for (int t = 0; t < w->nids && c->nids < PY_MAX_SENTENCE_SYLLABLES; t++)
                c->ids[c->nids++] = w->ids[t];
        }
        c->end = rawlen;
        c->score = best[n];
        count++;
    }
    /* The words at the start. */
    struct wedge *start[MAX_WEDGES];
    int nstart = 0;
    for (int i = 0; i < nwedges; i++)
        if (wedges[i].from == 0)
            start[nstart++] = &wedges[i];
    qsort(start, (size_t)nstart, sizeof start[0], by_length_then_score);
    for (int i = 0; i < nstart && count < max; i++) {
        int seen = 0;
        for (int k = 0; k < count && !seen; k++)
            seen = strcmp(out[k].text, start[i]->text) == 0;
        if (seen)
            continue;
        struct py_cand *c = &out[count++];
        strlcpy(c->text, start[i]->text, sizeof c->text);
        memcpy(c->ids, start[i]->ids, (size_t)start[i]->nids * sizeof c->ids[0]);
        c->nids = start[i]->nids;
        c->end = start[i]->to == n ? rawlen : rawpos[start[i]->to];
        c->score = start[i]->score;
    }
    return count;
}
