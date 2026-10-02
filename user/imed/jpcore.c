/* The Japanese core of imed (I4, docs/design/ime.md).
 *
 * Every part of the reading that is a reading of the dictionary or of the
 * user history becomes a node with the cost of its word.  A Viterbi search
 * finds the path of nodes with the lowest sum of word costs and
 * connection costs between the classes of neighbouring words, from the
 * start of the sentence to its end.  A character without a word becomes a
 * node of its own with a high cost, so a path always exists.  The path
 * falls into segments: a word that is not a particle, an auxiliary verb, a
 * suffix or a dependent word starts a segment, unless a prefix comes
 * before it.
 *
 * The state below belongs to the single thread of imed. */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include "jpcore.h"

#define MAX_NODES 8192
#define MAX_READING_CHARS 16    /* the longest reading of one word */
#define MAX_USER 2048
#define UNKNOWN_COST 15000
#define INF 0x3fffffff

enum { F_FUNCTION = 1, F_SUFFIX = 2, F_PREFIX = 4 };

struct node {
    int start, end;
    int lclass, rclass, cost;
    char surface[96];
    int best, back;
    int next_end;               /* the next node with the same end, or -1 */
};

struct user_word {
    char reading[96], surface[96];
    int lclass, rclass, count;
};

static const unsigned char *data;
static uint32_t nclass, nreadings, nentries, bos, noun;
static const unsigned char *flags, *matrix, *readings, *entries;
static const char *pool;

static struct user_word users[MAX_USER];
static int nusers;
static char user_path[256];

static struct node nodes[MAX_NODES];
static int nnodes;
static int end_head[JP_MAX_CHARS * 4 + 1];    /* the last node that ends at each byte offset, or -1 */

/* ---- the dictionary ---- */

static uint32_t u32(const unsigned char *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* The connection costs are stored in steps of 60. */
static int connection(int left, int right)
{
    return 60 * matrix[(size_t)left * nclass + (size_t)right];
}

/* The dictionary is mapped, so its pages are read when they are used. */
int jp_load(const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -errno;
    struct stat st;
    long size = fstat(fd, &st) == 0 ? (long)st.st_size : 0;
    void *map = size > 48 ? mmap(NULL, (size_t)size, PROT_READ, MAP_PRIVATE, fd, 0) : MAP_FAILED;
    close(fd);
    if (map == MAP_FAILED)
        return -EINVAL;
    data = map;
    if (memcmp(data, "MJP1", 4) != 0) {
        munmap(map, (size_t)size);
        data = NULL;
        return -EINVAL;
    }
    nclass = u32(data + 4);
    nreadings = u32(data + 8);
    nentries = u32(data + 12);
    bos = u32(data + 16);
    uint32_t flag_off = u32(data + 20), matrix_off = u32(data + 24), reading_off = u32(data + 28);
    uint32_t entry_off = u32(data + 32), pool_off = u32(data + 36), pool_size = u32(data + 40);
    noun = u32(data + 44);
    if ((size_t)pool_off + pool_size > (size_t)size || (size_t)entry_off + (size_t)nentries * 10 > (size_t)size ||
        (size_t)matrix_off + (size_t)nclass * nclass > (size_t)size || bos >= nclass || noun >= nclass) {
        munmap((void *)data, (size_t)size);
        data = NULL;
        return -EINVAL;
    }
    flags = data + flag_off;
    matrix = data + matrix_off;
    readings = data + reading_off;
    entries = data + entry_off;
    pool = (const char *)data + pool_off;
    return 0;
}

/* find_reading returns the number of the reading s[0..len), or -1. */
static int find_reading(const char *s, int len)
{
    int a = 0, b = (int)nreadings - 1;
    while (a <= b) {
        int m = (a + b) / 2;
        const char *r = pool + u32(readings + (size_t)m * 8);
        int c = strncmp(r, s, (size_t)len);
        if (c == 0)
            c = r[len] ? 1 : 0;
        if (c == 0)
            return m;
        if (c < 0)
            a = m + 1;
        else
            b = m - 1;
    }
    return -1;
}

static void entry_range(int reading, uint32_t *first, uint32_t *end)
{
    *first = u32(readings + (size_t)reading * 8 + 4);
    *end = (uint32_t)reading + 1 < nreadings ? u32(readings + (size_t)(reading + 1) * 8 + 4) : nentries;
}

/* entry_surface writes the surface of entry e of the reading s[0..len). */
static void entry_surface(uint32_t e, const char *s, int len, char *out, size_t size)
{
    uint32_t where = u32(entries + (size_t)e * 10);
    char reading[96];
    size_t n = (size_t)len < sizeof reading - 1 ? (size_t)len : sizeof reading - 1;
    memcpy(reading, s, n);
    reading[n] = '\0';
    if (where == 0xffffffff)
        strlcpy(out, reading, size);
    else if (where == 0xfffffffe)
        jp_katakana(reading, out, size);
    else
        strlcpy(out, pool + where, size);
}

static int entry_lclass(uint32_t e) { const unsigned char *p = entries + (size_t)e * 10 + 4; return p[0] | p[1] << 8; }
static int entry_rclass(uint32_t e) { const unsigned char *p = entries + (size_t)e * 10 + 6; return p[0] | p[1] << 8; }
static int entry_cost(uint32_t e) { const unsigned char *p = entries + (size_t)e * 10 + 8; return (int16_t)(p[0] | p[1] << 8); }

/* ---- the user history ---- */

static struct user_word *user_find(const char *reading, const char *surface)
{
    for (int i = 0; i < nusers; i++)
        if (strcmp(users[i].reading, reading) == 0 && strcmp(users[i].surface, surface) == 0)
            return &users[i];
    return NULL;
}

/* The file has one line "reading TAB surface TAB left class TAB right
 * class TAB count" for each choice.  A later line replaces the count. */
int jp_user_load(const char *path)
{
    strlcpy(user_path, path, sizeof user_path);
    nusers = 0;
    FILE *f = fopen(path, "r");
    if (!f)
        return errno == ENOENT ? 0 : -errno;
    char line[320];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\n")] = '\0';
        char *field[5];
        int k = 0;
        for (char *p = line; k < 5; k++) {
            field[k] = p;
            char *tab = strchr(p, '\t');
            if (!tab) {
                k++;
                break;
            }
            *tab = '\0';
            p = tab + 1;
        }
        if (k < 5 || !field[0][0] || !field[1][0])
            continue;
        int lc = atoi(field[2]), rc = atoi(field[3]);
        if (lc < 0 || rc < 0 || (uint32_t)lc >= nclass || (uint32_t)rc >= nclass)
            continue;
        struct user_word *u = user_find(field[0], field[1]);
        if (!u && nusers < MAX_USER) {
            u = &users[nusers++];
            strlcpy(u->reading, field[0], sizeof u->reading);
            strlcpy(u->surface, field[1], sizeof u->surface);
        }
        if (u) {
            u->lclass = lc;
            u->rclass = rc;
            u->count = atoi(field[4]);
        }
    }
    fclose(f);
    return 0;
}

void jp_user_learn(const char *reading, const char *surface, int lclass, int rclass)
{
    if (!reading[0] || !surface[0] || lclass < 0 || rclass < 0)
        return;
    struct user_word *u = user_find(reading, surface);
    if (!u) {
        if (nusers == MAX_USER)
            return;
        u = &users[nusers++];
        strlcpy(u->reading, reading, sizeof u->reading);
        strlcpy(u->surface, surface, sizeof u->surface);
        u->count = 0;
    }
    u->lclass = lclass;
    u->rclass = rclass;
    u->count++;
    FILE *f = user_path[0] ? fopen(user_path, "a") : NULL;
    if (f) {
        fprintf(f, "%s\t%s\t%d\t%d\t%d\n", reading, surface, lclass, rclass, u->count);
        fclose(f);
    }
}

/* A chosen word costs less with each choice. */
static int user_cost(int count)
{
    return 2500 - 300 * (count < 8 ? count : 8);
}

/* ---- the lattice ---- */

static int char_len(const char *s)
{
    unsigned char c = (unsigned char)*s;
    return c < 0x80 ? 1 : c < 0xe0 ? 2 : c < 0xf0 ? 3 : 4;
}

static void add_node(int start, int end, int lclass, int rclass, int cost, const char *surface)
{
    if (nnodes == MAX_NODES)
        return;
    struct node *n = &nodes[nnodes++];
    n->start = start;
    n->end = end;
    n->lclass = lclass;
    n->rclass = rclass;
    n->cost = cost;
    strlcpy(n->surface, surface, sizeof n->surface);
    n->best = INF;
    n->back = -1;
    n->next_end = end_head[end];
    end_head[end] = nnodes - 1;
}

/* build adds the nodes of reading[from..to): the words of the dictionary
 * and of the user history, and each character alone. */
static void build(const char *reading, int from, int to)
{
    nnodes = 0;
    for (int i = 0; i <= JP_MAX_CHARS * 4; i++)
        end_head[i] = -1;
    if (to > JP_MAX_CHARS * 4)
        to = JP_MAX_CHARS * 4;
    for (int i = from; i < to; i += char_len(reading + i)) {
        int clen = char_len(reading + i);
        char one[8];
        memcpy(one, reading + i, (size_t)clen);
        one[clen] = '\0';
        add_node(i, i + clen, (int)noun, (int)noun, UNKNOWN_COST, one);
        int j = i, chars = 0;
        while (j < to && chars < MAX_READING_CHARS) {
            j += char_len(reading + j);
            chars++;
            int len = j - i, r = data ? find_reading(reading + i, len) : -1;
            if (r >= 0) {
                uint32_t first, end;
                entry_range(r, &first, &end);
                for (uint32_t e = first; e < end; e++) {
                    char surface[96];
                    entry_surface(e, reading + i, len, surface, sizeof surface);
                    add_node(i, j, entry_lclass(e), entry_rclass(e), entry_cost(e), surface);
                }
            }
            for (int u = 0; u < nusers; u++)
                if ((int)strlen(users[u].reading) == len && strncmp(users[u].reading, reading + i, (size_t)len) == 0)
                    add_node(i, j, users[u].lclass, users[u].rclass, user_cost(users[u].count), users[u].surface);
        }
    }
}

/* viterbi finds the lowest cost path of the nodes from from to to and
 * returns the index of its last node, or -1. */
static int viterbi(int from, int to)
{
    for (int k = 0; k < nnodes; k++) {
        struct node *n = &nodes[k];
        if (n->start == from) {
            n->best = connection((int)bos, n->lclass) + n->cost;
            continue;
        }
        for (int p = end_head[n->start]; p >= 0; p = nodes[p].next_end) {
            struct node *q = &nodes[p];
            if (q->best >= INF)
                continue;
            int v = q->best + connection(q->rclass, n->lclass) + n->cost;
            if (v < n->best) {
                n->best = v;
                n->back = p;
            }
        }
    }
    int last = -1, best = INF;
    for (int k = end_head[to]; k >= 0; k = nodes[k].next_end)
        if (nodes[k].best < INF) {
            int v = nodes[k].best + connection(nodes[k].rclass, (int)bos);
            if (v < best) {
                best = v;
                last = k;
            }
        }
    return last;
}

static int path_of(int last, int *path, int max)
{
    int n = 0;
    for (int k = last; k >= 0 && n < max; k = nodes[k].back)
        path[n++] = k;
    for (int i = 0; i < n / 2; i++) {
        int t = path[i];
        path[i] = path[n - 1 - i];
        path[n - 1 - i] = t;
    }
    return n;
}

static int starts_segment(int k, int prev)
{
    if (prev < 0)
        return 1;
    return !(flags[nodes[k].lclass] & (F_FUNCTION | F_SUFFIX)) && !(flags[nodes[prev].rclass] & F_PREFIX);
}

/* segments_of divides the path into segments. */
static int segments_of(const int *path, int n, struct jp_segment *out, int max)
{
    int count = 0;
    for (int i = 0; i < n; i++) {
        int k = path[i];
        if (starts_segment(k, i ? path[i - 1] : -1) || count == 0) {
            if (count == max)
                break;
            struct jp_segment *s = &out[count++];
            s->start = nodes[k].start;
            s->surface[0] = '\0';
            s->lclass = nodes[k].lclass;
        }
        struct jp_segment *s = &out[count - 1];
        s->end = nodes[k].end;
        s->rclass = nodes[k].rclass;
        strlcat(s->surface, nodes[k].surface, sizeof s->surface);
    }
    return count;
}

int jp_convert(const char *reading, int from, int first_end, struct jp_segment *out, int max)
{
    int len = (int)strlen(reading), count = 0;
    int path[JP_MAX_CHARS];
    if (from >= len || max <= 0)
        return 0;
    if (first_end > from && first_end < len) {
        build(reading, from, first_end);
        int last = viterbi(from, first_end);
        int n = path_of(last, path, JP_MAX_CHARS);
        struct jp_segment *s = &out[count++];
        s->start = from;
        s->end = first_end;
        s->surface[0] = '\0';
        for (int i = 0; i < n; i++)
            strlcat(s->surface, nodes[path[i]].surface, sizeof s->surface);
        s->lclass = n ? nodes[path[0]].lclass : (int)noun;
        s->rclass = n ? nodes[path[n - 1]].rclass : (int)noun;
        from = first_end;
    }
    build(reading, from, len);
    int last = viterbi(from, len);
    int n = path_of(last, path, JP_MAX_CHARS);
    return count + segments_of(path, n, out + count, max - count);
}

static int add_cand(struct jp_cand *out, int count, int max, const char *surface, int lclass, int rclass)
{
    if (count >= max || !surface[0])
        return count;
    for (int i = 0; i < count; i++)
        if (strcmp(out[i].surface, surface) == 0)
            return count;
    strlcpy(out[count].surface, surface, sizeof out[count].surface);
    out[count].lclass = lclass;
    out[count].rclass = rclass;
    return count + 1;
}

int jp_candidates(const char *reading, int start, int end, struct jp_cand *out, int max)
{
    int count = 0, len = end - start;
    char span[JP_SURFACE];
    if (len <= 0 || (size_t)len >= sizeof span)
        return 0;
    memcpy(span, reading + start, (size_t)len);
    span[len] = '\0';
    build(reading, start, end);
    int path[JP_MAX_CHARS], n = path_of(viterbi(start, end), path, JP_MAX_CHARS);
    char best[JP_SURFACE] = "";
    for (int i = 0; i < n; i++)
        strlcat(best, nodes[path[i]].surface, sizeof best);
    if (n)
        count = add_cand(out, count, max, best, nodes[path[0]].lclass, nodes[path[n - 1]].rclass);
    /* The words of the whole reading, the chosen ones first. */
    for (int u = 0; u < nusers; u++)
        if (strcmp(users[u].reading, span) == 0)
            count = add_cand(out, count, max, users[u].surface, users[u].lclass, users[u].rclass);
    int r = data ? find_reading(span, len) : -1;
    if (r >= 0) {
        uint32_t first, last;
        entry_range(r, &first, &last);
        for (uint32_t e = first; e < last; e++) {
            char surface[96];
            entry_surface(e, span, len, surface, sizeof surface);
            count = add_cand(out, count, max, surface, entry_lclass(e), entry_rclass(e));
        }
    }
    /* The other words of the first word, with the rest of the path. */
    if (n > 1) {
        struct node first = nodes[path[0]];
        char rest[JP_SURFACE] = "";
        for (int i = 1; i < n; i++)
            strlcat(rest, nodes[path[i]].surface, sizeof rest);
        int rclass = nodes[path[n - 1]].rclass;
        int fr = data ? find_reading(reading + first.start, first.end - first.start) : -1;
        if (fr >= 0) {
            uint32_t a, b;
            entry_range(fr, &a, &b);
            for (uint32_t e = a; e < b; e++) {
                char surface[96], joined[JP_SURFACE];
                entry_surface(e, reading + first.start, first.end - first.start, surface, sizeof surface);
                snprintf(joined, sizeof joined, "%s%s", surface, rest);
                count = add_cand(out, count, max, joined, entry_lclass(e), rclass);
            }
        }
    }
    char kata[JP_SURFACE];
    jp_katakana(span, kata, sizeof kata);
    count = add_cand(out, count, max, span, (int)noun, (int)noun);
    count = add_cand(out, count, max, kata, (int)noun, (int)noun);
    return count;
}

/* ---- forms ---- */

static int decode(const char *s, uint32_t *cp)
{
    const unsigned char *p = (const unsigned char *)s;
    if (p[0] < 0x80) { *cp = p[0]; return 1; }
    if (p[0] < 0xe0 && p[1]) { *cp = (uint32_t)(p[0] & 0x1f) << 6 | (p[1] & 0x3f); return 2; }
    if (p[0] < 0xf0 && p[1] && p[2]) { *cp = (uint32_t)(p[0] & 0x0f) << 12 | (uint32_t)(p[1] & 0x3f) << 6 | (p[2] & 0x3f); return 3; }
    if (p[1] && p[2] && p[3]) {
        *cp = (uint32_t)(p[0] & 7) << 18 | (uint32_t)(p[1] & 0x3f) << 12 | (uint32_t)(p[2] & 0x3f) << 6 | (p[3] & 0x3f);
        return 4;
    }
    *cp = '?';
    return 1;
}

static size_t encode(uint32_t cp, char *out)
{
    if (cp < 0x80) { out[0] = (char)cp; return 1; }
    if (cp < 0x800) { out[0] = (char)(0xc0 | cp >> 6); out[1] = (char)(0x80 | (cp & 0x3f)); return 2; }
    if (cp < 0x10000) {
        out[0] = (char)(0xe0 | cp >> 12); out[1] = (char)(0x80 | (cp >> 6 & 0x3f)); out[2] = (char)(0x80 | (cp & 0x3f));
        return 3;
    }
    out[0] = (char)(0xf0 | cp >> 18); out[1] = (char)(0x80 | (cp >> 12 & 0x3f));
    out[2] = (char)(0x80 | (cp >> 6 & 0x3f)); out[3] = (char)(0x80 | (cp & 0x3f));
    return 4;
}

static void append(char *out, size_t size, size_t *o, const char *s, size_t n)
{
    if (*o + n + 1 > size)
        return;
    memcpy(out + *o, s, n);
    *o += n;
    out[*o] = '\0';
}

void jp_katakana(const char *in, char *out, size_t size)
{
    size_t o = 0;
    out[0] = '\0';
    while (*in) {
        uint32_t cp;
        in += decode(in, &cp);
        if (cp >= 0x3041 && cp <= 0x3096)
            cp += 0x60;
        char buf[4];
        append(out, size, &o, buf, encode(cp, buf));
    }
}

/* The half width forms of the katakana U+30A1 to U+30F6. */
static const char *const half_kana[] = {
    "ｧ", "ｱ", "ｨ", "ｲ", "ｩ", "ｳ", "ｪ", "ｴ", "ｫ", "ｵ", "ｶ", "ｶﾞ", "ｷ", "ｷﾞ", "ｸ", "ｸﾞ", "ｹ", "ｹﾞ", "ｺ", "ｺﾞ",
    "ｻ", "ｻﾞ", "ｼ", "ｼﾞ", "ｽ", "ｽﾞ", "ｾ", "ｾﾞ", "ｿ", "ｿﾞ", "ﾀ", "ﾀﾞ", "ﾁ", "ﾁﾞ", "ｯ", "ﾂ", "ﾂﾞ", "ﾃ", "ﾃﾞ",
    "ﾄ", "ﾄﾞ", "ﾅ", "ﾆ", "ﾇ", "ﾈ", "ﾉ", "ﾊ", "ﾊﾞ", "ﾊﾟ", "ﾋ", "ﾋﾞ", "ﾋﾟ", "ﾌ", "ﾌﾞ", "ﾌﾟ", "ﾍ", "ﾍﾞ", "ﾍﾟ",
    "ﾎ", "ﾎﾞ", "ﾎﾟ", "ﾏ", "ﾐ", "ﾑ", "ﾒ", "ﾓ", "ｬ", "ﾔ", "ｭ", "ﾕ", "ｮ", "ﾖ", "ﾗ", "ﾘ", "ﾙ", "ﾚ", "ﾛ", "ﾜ", "ﾜ",
    "ｲ", "ｴ", "ｦ", "ﾝ", "ｳﾞ", "ｶ", "ｹ",
};

void jp_halfwidth(const char *in, char *out, size_t size)
{
    size_t o = 0;
    out[0] = '\0';
    while (*in) {
        uint32_t cp;
        in += decode(in, &cp);
        if (cp >= 0x3041 && cp <= 0x3096)
            cp += 0x60;
        const char *s = NULL;
        char buf[4];
        if (cp >= 0x30a1 && cp <= 0x30f6)
            s = half_kana[cp - 0x30a1];
        else if (cp == 0x30fc) s = "ｰ";
        else if (cp == 0x3002) s = "｡";
        else if (cp == 0x3001) s = "､";
        else if (cp == 0x300c) s = "｢";
        else if (cp == 0x300d) s = "｣";
        else if (cp == 0x30fb) s = "･";
        else if (cp == 0x3000) s = " ";
        else if (cp >= 0xff01 && cp <= 0xff5e)
            cp -= 0xfee0;
        if (s)
            append(out, size, &o, s, strlen(s));
        else
            append(out, size, &o, buf, encode(cp, buf));
    }
}

void jp_fullwidth(const char *in, char *out, size_t size)
{
    size_t o = 0;
    out[0] = '\0';
    while (*in) {
        uint32_t cp;
        in += decode(in, &cp);
        if (cp >= 0x21 && cp <= 0x7e)
            cp += 0xfee0;
        else if (cp == ' ')
            cp = 0x3000;
        char buf[4];
        append(out, size, &o, buf, encode(cp, buf));
    }
}
