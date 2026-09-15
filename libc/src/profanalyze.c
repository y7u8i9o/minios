/* Aggregation of the profiler's event stream: interned names, call trees
 * with two accumulated weights, flame graph layout, and the session that
 * turns records into the four views a front end shows.
 *
 * Nothing here talks to the device; a session is fed records and is the
 * only place that knows how each event type is charged. */
#include <minios/profile.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- interned names ---- */

#define NAME_BUCKETS 2048

struct name_entry {
    char *text;
    int next;                   /* index of the next name in the bucket, -1 */
};

struct prof_names {
    struct name_entry *entries;
    int count, cap;
    int buckets[NAME_BUCKETS];
};

static unsigned hash_string(const char *s)
{
    unsigned h = 2166136261u;
    while (*s) {
        h ^= (unsigned char)*s++;
        h *= 16777619u;
    }
    return h;
}

struct prof_names *prof_names_new(void)
{
    struct prof_names *n = calloc(1, sizeof *n);
    if (!n)
        return NULL;
    for (int i = 0; i < NAME_BUCKETS; i++)
        n->buckets[i] = -1;
    if (prof_names_intern(n, "") != 0) {
        prof_names_free(n);
        return NULL;
    }
    return n;
}

void prof_names_free(struct prof_names *n)
{
    if (!n)
        return;
    for (int i = 0; i < n->count; i++)
        free(n->entries[i].text);
    free(n->entries);
    free(n);
}

int prof_names_intern(struct prof_names *n, const char *s)
{
    if (!n || !s)
        return 0;
    unsigned b = hash_string(s) % NAME_BUCKETS;
    for (int i = n->buckets[b]; i >= 0; i = n->entries[i].next)
        if (strcmp(n->entries[i].text, s) == 0)
            return i;
    if (n->count == n->cap) {
        int cap = n->cap ? n->cap * 2 : 256;
        struct name_entry *e = realloc(n->entries, (size_t)cap * sizeof *e);
        if (!e)
            return 0;
        n->entries = e;
        n->cap = cap;
    }
    char *copy = strdup(s);
    if (!copy)
        return 0;
    n->entries[n->count].text = copy;
    n->entries[n->count].next = n->buckets[b];
    n->buckets[b] = n->count;
    return n->count++;
}

const char *prof_names_get(const struct prof_names *n, int id)
{
    return n && id >= 0 && id < n->count ? n->entries[id].text : "";
}

/* ---- call tree ---- */

int prof_tree_init(struct prof_tree *t, struct prof_names *names)
{
    memset(t, 0, sizeof *t);
    t->names = names;
    t->cap = 256;
    t->nodes = calloc((size_t)t->cap, sizeof *t->nodes);
    if (!t->nodes)
        return -1;
    t->nodes[0].name = 0;
    t->nodes[0].parent = t->nodes[0].child = t->nodes[0].sibling = -1;
    t->count = 1;
    return 0;
}

void prof_tree_clear(struct prof_tree *t)
{
    if (!t->nodes)
        return;
    memset(t->nodes, 0, (size_t)t->cap * sizeof *t->nodes);
    t->nodes[0].parent = t->nodes[0].child = t->nodes[0].sibling = -1;
    t->count = 1;
}

void prof_tree_free(struct prof_tree *t)
{
    free(t->nodes);
    memset(t, 0, sizeof *t);
}

static int tree_child(struct prof_tree *t, int parent, int name, int kernel)
{
    for (int i = t->nodes[parent].child; i >= 0; i = t->nodes[i].sibling)
        if (t->nodes[i].name == name && t->nodes[i].kernel == (uint8_t)kernel)
            return i;
    if (t->count == t->cap) {
        int cap = t->cap * 2;
        struct prof_node *n = realloc(t->nodes, (size_t)cap * sizeof *n);
        if (!n)
            return -1;
        memset(n + t->cap, 0, (size_t)(cap - t->cap) * sizeof *n);
        t->nodes = n;
        t->cap = cap;
    }
    int index = t->count++;
    struct prof_node *n = &t->nodes[index];
    n->name = name;
    n->parent = parent;
    n->child = -1;
    n->kernel = (uint8_t)kernel;
    n->depth = t->nodes[parent].depth + 1;
    n->sibling = t->nodes[parent].child;
    t->nodes[parent].child = index;
    return index;
}

int prof_tree_add(struct prof_tree *t, const struct prof_stack *s, uint64_t weight, uint64_t extra)
{
    int node = 0;
    t->nodes[0].total += weight;
    t->nodes[0].extra += extra;
    for (int i = 0; i < s->count; i++) {
        int next = tree_child(t, node, s->names[i], s->kernel[i]);
        if (next < 0)
            break;
        node = next;
        t->nodes[node].total += weight;
        t->nodes[node].extra += extra;
    }
    t->nodes[node].self += weight;
    t->nodes[node].count++;
    return node;
}

void prof_tree_charge(struct prof_tree *t, int leaf, int64_t weight, int64_t extra)
{
    if (leaf < 0 || leaf >= t->count)
        return;
    t->nodes[leaf].self += (uint64_t)weight;
    for (int i = leaf; i >= 0; i = t->nodes[i].parent) {
        t->nodes[i].total += (uint64_t)weight;
        t->nodes[i].extra += (uint64_t)extra;
    }
}

struct sort_context {
    const struct prof_tree *tree;
};
static struct sort_context sort_ctx;

static int cmp_child(const void *a, const void *b)
{
    const struct prof_tree *t = sort_ctx.tree;
    int x = *(const int *)a, y = *(const int *)b;
    const char *nx = prof_names_get(t->names, t->nodes[x].name);
    const char *ny = prof_names_get(t->names, t->nodes[y].name);
    int r = strcmp(nx, ny);
    return r ? r : x - y;
}

void prof_tree_sort(struct prof_tree *t)
{
    int *order = malloc((size_t)t->count * sizeof *order);
    if (!order)
        return;
    sort_ctx.tree = t;
    for (int i = 0; i < t->count; i++) {
        int n = 0;
        for (int c = t->nodes[i].child; c >= 0; c = t->nodes[c].sibling)
            order[n++] = c;
        if (n < 2)
            continue;
        qsort(order, (size_t)n, sizeof *order, cmp_child);
        t->nodes[i].child = order[0];
        for (int k = 0; k < n - 1; k++)
            t->nodes[order[k]].sibling = order[k + 1];
        t->nodes[order[n - 1]].sibling = -1;
    }
    free(order);
}

int prof_tree_path(const struct prof_tree *t, int node, const char **names, int max)
{
    int n = 0;
    for (int i = node; i > 0; i = t->nodes[i].parent)
        n++;
    if (n > max)
        n = max;
    int at = n;
    for (int i = node; i > 0 && at > 0; i = t->nodes[i].parent)
        names[--at] = prof_names_get(t->names, t->nodes[i].name);
    return n;
}

/* ---- flame graph ---- */

size_t prof_flame_layout(const struct prof_tree *t, int root, int use_extra,
                         struct prof_flame_box *out, size_t max)
{
    if (!t->nodes || root < 0 || root >= t->count)
        return 0;
    struct frame {
        int node;
        uint64_t start;
    };
    struct frame *stack = malloc((size_t)t->count * sizeof *stack);
    if (!stack)
        return 0;
    size_t written = 0;
    int top = 0;
    int base_depth = t->nodes[root].depth;
    stack[top++] = (struct frame){ root, 0 };
    while (top && written < max) {
        struct frame f = stack[--top];
        const struct prof_node *n = &t->nodes[f.node];
        uint64_t width = use_extra ? n->extra : n->total;
        if (!width)
            continue;
        out[written].node = f.node;
        out[written].depth = n->depth - base_depth;
        out[written].start = f.start;
        out[written].width = width;
        written++;
        /* Each child gets its slice left to right; they are then pushed
         * in reverse so the leftmost is the next one visited. */
        uint64_t at = f.start;
        int first = top;
        for (int c = n->child; c >= 0; c = t->nodes[c].sibling) {
            uint64_t w = use_extra ? t->nodes[c].extra : t->nodes[c].total;
            if (!w)
                continue;
            stack[top].node = c;
            stack[top].start = at;
            at += w;
            top++;
        }
        for (int i = first, j = top - 1; i < j; i++, j--) {
            struct frame swap = stack[i];
            stack[i] = stack[j];
            stack[j] = swap;
        }
    }
    free(stack);
    return written;
}

/* ---- session ---- */

struct prof_live {                  /* one outstanding kernel allocation */
    uint64_t addr;
    uint64_t bytes;
    int node;                       /* leaf in the heap tree */
};

struct prof_pending {               /* a thread between its block and its run */
    uint32_t tid;
    int used;
    int node;                       /* leaf in the off CPU tree */
};

#define PENDING_SLOTS 512

static struct prof_thread_stat *thread_stat(struct prof_session *s, uint32_t pid, uint32_t tid)
{
    for (size_t i = 0; i < s->nthreads; i++)
        if (s->threads[i].tid == tid)
            return &s->threads[i];
    if (s->nthreads == s->threads_cap) {
        size_t cap = s->threads_cap ? s->threads_cap * 2 : 64;
        struct prof_thread_stat *t = realloc(s->threads, cap * sizeof *t);
        if (!t)
            return NULL;
        s->threads = t;
        s->threads_cap = cap;
    }
    struct prof_thread_stat *t = &s->threads[s->nthreads++];
    memset(t, 0, sizeof *t);
    t->pid = pid;
    t->tid = tid;
    t->name = prof_resolver_procname(s->res, (pid_t)pid);
    return t;
}

struct prof_session *prof_session_new(struct prof_resolver *res, uint64_t period_ns)
{
    struct prof_session *s = calloc(1, sizeof *s);
    if (!s)
        return NULL;
    s->res = res;
    s->period_ns = period_ns ? period_ns : 1000000;
    s->names = prof_names_new();
    s->live_cap = 4096;
    s->live = calloc(s->live_cap, sizeof *s->live);
    s->pending_cap = PENDING_SLOTS;
    s->pending = calloc(s->pending_cap, sizeof *s->pending);
    if (!s->names || !s->live || !s->pending) {
        prof_session_free(s);
        return NULL;
    }
    for (int i = 0; i < PROF_VIEW_COUNT; i++) {
        if (prof_tree_init(&s->view[i], s->names) < 0) {
            prof_session_free(s);
            return NULL;
        }
        prof_hist_init(&s->flat[i]);
    }
    return s;
}

void prof_session_free(struct prof_session *s)
{
    if (!s)
        return;
    for (int i = 0; i < PROF_VIEW_COUNT; i++) {
        prof_tree_free(&s->view[i]);
        prof_hist_clear(&s->flat[i]);
    }
    prof_names_free(s->names);
    free(s->threads);
    free(s->live);
    free(s->pending);
    free(s);
}

void prof_session_reset(struct prof_session *s)
{
    for (int i = 0; i < PROF_VIEW_COUNT; i++) {
        prof_tree_clear(&s->view[i]);
        prof_hist_clear(&s->flat[i]);
        prof_hist_init(&s->flat[i]);
    }
    memset(s->live, 0, s->live_cap * sizeof *s->live);
    memset(s->pending, 0, s->pending_cap * sizeof *s->pending);
    s->nthreads = 0;
    s->live_used = 0;
    s->events = s->first_ns = s->last_ns = 0;
    s->live_bytes = s->live_peak = s->alloc_bytes = s->freed_bytes = 0;
    s->live_count = 0;
    s->locked_samples = 0;
    memset(s->counts, 0, sizeof s->counts);
}

/* ---- live allocations ---- */

static size_t live_slot(const struct prof_session *s, uint64_t addr)
{
    uint64_t h = addr * 0x9e3779b97f4a7c15ULL;
    return (size_t)(h >> 40) & (s->live_cap - 1);
}

static int live_grow(struct prof_session *s)
{
    size_t cap = s->live_cap * 2;
    struct prof_live *table = calloc(cap, sizeof *table);
    if (!table)
        return -1;
    struct prof_live *old = s->live;
    size_t old_cap = s->live_cap;
    s->live = table;
    s->live_cap = cap;
    for (size_t i = 0; i < old_cap; i++) {
        if (!old[i].addr)
            continue;
        size_t slot = live_slot(s, old[i].addr);
        while (table[slot].addr)
            slot = (slot + 1) & (cap - 1);
        table[slot] = old[i];
    }
    free(old);
    return 0;
}

static void live_insert(struct prof_session *s, uint64_t addr, uint64_t bytes, int node)
{
    if (!addr)
        return;
    if ((s->live_used + 1) * 4 > s->live_cap * 3 && live_grow(s) < 0)
        return;
    size_t slot = live_slot(s, addr);
    while (s->live[slot].addr && s->live[slot].addr != addr)
        slot = (slot + 1) & (s->live_cap - 1);
    if (!s->live[slot].addr)
        s->live_used++;
    s->live[slot].addr = addr;
    s->live[slot].bytes = bytes;
    s->live[slot].node = node;
    s->live_count = s->live_used;
}

/* Remove addr, reporting what it held. Deletion rehashes the run that
 * follows so the open addressing stays searchable. */
static int live_remove(struct prof_session *s, uint64_t addr, uint64_t *bytes, int *node)
{
    size_t slot = live_slot(s, addr);
    size_t guard = 0;
    while (s->live[slot].addr != addr) {
        if (!s->live[slot].addr || ++guard > s->live_cap)
            return -1;
        slot = (slot + 1) & (s->live_cap - 1);
    }
    *bytes = s->live[slot].bytes;
    *node = s->live[slot].node;
    memset(&s->live[slot], 0, sizeof s->live[slot]);
    s->live_used--;
    /* Reinsert the run that follows, which may have probed past the hole
     * this deletion opened. */
    size_t next = (slot + 1) & (s->live_cap - 1);
    while (s->live[next].addr) {
        struct prof_live moved = s->live[next];
        memset(&s->live[next], 0, sizeof s->live[next]);
        s->live_used--;
        live_insert(s, moved.addr, moved.bytes, moved.node);
        next = (next + 1) & (s->live_cap - 1);
    }
    s->live_count = s->live_used;
    return 0;
}

static struct prof_pending *pending_slot(struct prof_session *s, uint32_t tid, int create)
{
    size_t start = (size_t)(tid * 2654435761u) & (s->pending_cap - 1);
    for (size_t i = 0; i < s->pending_cap; i++) {
        struct prof_pending *p = &s->pending[(start + i) & (s->pending_cap - 1)];
        if (p->used && p->tid == tid)
            return p;
        if (!p->used) {
            if (!create)
                return NULL;
            p->used = 1;
            p->tid = tid;
            p->node = -1;
            return p;
        }
    }
    return NULL;
}

/* ---- events ---- */

static void build_stack(struct prof_session *s, const struct prof_event *e, struct prof_stack *st)
{
    uint64_t addr[PROF_MAX_FRAMES];
    uint8_t kern[PROF_MAX_FRAMES];
    int n = 0;
    int in_kernel = !(e->flags & PROF_FLAG_USER);
    for (unsigned i = 0; i < e->depth && n < PROF_MAX_FRAMES; i++) {
        if (e->chain[i] == PROF_FRAME_BOUNDARY) {
            in_kernel = 0;      /* the frames that follow are user frames */
            continue;
        }
        addr[n] = e->chain[i];
        kern[n] = (uint8_t)in_kernel;
        n++;
    }
    st->count = 0;
    for (int i = n - 1; i >= 0; i--) {
        st->names[st->count] = prof_names_intern(s->names,
                                                 prof_resolve(s->res, (pid_t)e->pid, kern[i], addr[i]));
        st->kernel[st->count] = kern[i];
        st->count++;
    }
}

/* The innermost frame that is not a lock primitive, which is where a
 * sample taken with interrupts disabled really belongs. */
const char *prof_session_leaf(struct prof_session *s, const struct prof_event *e, int *locked)
{
    int in_kernel = !(e->flags & PROF_FLAG_USER);
    if (locked)
        *locked = 0;
    for (unsigned i = 0; i < e->depth; i++) {
        if (e->chain[i] == PROF_FRAME_BOUNDARY) {
            in_kernel = 0;
            continue;
        }
        const char *name = prof_resolve(s->res, (pid_t)e->pid, in_kernel, e->chain[i]);
        if (in_kernel && prof_is_lock_primitive(name)) {
            if (locked)
                *locked = 1;
            continue;
        }
        return name;
    }
    return "?";
}

static void add_view(struct prof_session *s, int view, const struct prof_event *e,
                     uint64_t weight, uint64_t extra, int *leaf_out)
{
    struct prof_stack st;
    build_stack(s, e, &st);
    int leaf = prof_tree_add(&s->view[view], &st, weight, extra);
    if (leaf_out)
        *leaf_out = leaf;
    int locked = 0;
    const char *name = prof_session_leaf(s, e, &locked);
    if (locked)
        s->locked_samples++;
    prof_hist_add_weight(&s->flat[view], name, !(e->flags & PROF_FLAG_USER), weight, extra);
}

int prof_session_event(struct prof_session *s, const struct prof_event *e)
{
    if (e->type >= PROF_EV_TYPES)
        return -1;
    if (s->exclude && (pid_t)e->pid == s->exclude)
        return -1;
    s->events++;
    s->counts[e->type]++;
    if (!s->first_ns)
        s->first_ns = e->time_ns;
    s->last_ns = e->time_ns;
    struct prof_thread_stat *ts = thread_stat(s, e->pid, e->tid);

    switch (e->type) {
    case PROF_EV_SAMPLE:
        add_view(s, PROF_VIEW_CPU, e, s->period_ns, 0, NULL);
        if (ts)
            ts->samples++;
        break;
    case PROF_EV_BLOCK: {
        int leaf = -1;
        /* The time is unknown until the thread runs again, so the stack is
         * recorded now with no weight and charged by the run event. */
        add_view(s, PROF_VIEW_OFFCPU, e, 0, 0, &leaf);
        struct prof_pending *p = pending_slot(s, e->tid, 1);
        if (p)
            p->node = leaf;
        if (ts) {
            ts->blocks++;
            ts->on_cpu_ns += e->a;
            if (e->flags & PROF_FLAG_PREEMPT)
                ts->preempts++;
        }
        break;
    }
    case PROF_EV_RUN: {
        struct prof_pending *p = pending_slot(s, e->tid, 0);
        if (p && p->node >= 0) {
            prof_tree_charge(&s->view[PROF_VIEW_OFFCPU], p->node, (int64_t)e->a, 0);
            const struct prof_tree *t = &s->view[PROF_VIEW_OFFCPU];
            prof_hist_add_weight(&s->flat[PROF_VIEW_OFFCPU],
                                 prof_names_get(t->names, t->nodes[p->node].name),
                                 t->nodes[p->node].kernel, e->a, e->b);
            p->node = -1;
        }
        if (ts) {
            ts->off_cpu_ns += e->a;
            ts->ready_ns += e->b;
        }
        break;
    }
    case PROF_EV_ALLOC: {
        int leaf = -1;
        add_view(s, PROF_VIEW_HEAP, e, e->b, e->b, &leaf);
        live_insert(s, e->a, e->b, leaf);
        s->alloc_bytes += e->b;
        s->live_bytes += e->b;
        if (s->live_bytes > s->live_peak)
            s->live_peak = s->live_bytes;
        if (ts) {
            ts->allocs++;
            ts->alloc_bytes += e->b;
        }
        break;
    }
    case PROF_EV_FREE: {
        uint64_t bytes = 0;
        int node = -1;
        if (live_remove(s, e->a, &bytes, &node) == 0) {
            prof_tree_charge(&s->view[PROF_VIEW_HEAP], node, 0, -(int64_t)bytes);
            s->freed_bytes += bytes;
            s->live_bytes -= bytes < s->live_bytes ? bytes : s->live_bytes;
        }
        break;
    }
    case PROF_EV_IO:
        add_view(s, PROF_VIEW_IO, e, e->b, e->a, NULL);
        if (ts) {
            ts->ios++;
            ts->io_ns += e->b;
            ts->io_bytes += e->a;
        }
        break;
    default:
        return -1;
    }
    return 0;
}

size_t prof_session_feed(struct prof_session *s, const void *buf, size_t bytes)
{
    size_t n = 0;
    for (const struct prof_event *e = prof_event_first(buf, bytes); e;
         e = prof_event_next(buf, bytes, e)) {
        prof_session_event(s, e);
        n++;
    }
    return n;
}

static int cmp_thread(const void *a, const void *b)
{
    const struct prof_thread_stat *x = a, *y = b;
    uint64_t xw = x->on_cpu_ns ? x->on_cpu_ns : x->samples;
    uint64_t yw = y->on_cpu_ns ? y->on_cpu_ns : y->samples;
    if (xw != yw)
        return xw < yw ? 1 : -1;
    return x->tid < y->tid ? -1 : x->tid > y->tid;
}

void prof_session_sort_threads(struct prof_session *s)
{
    qsort(s->threads, s->nthreads, sizeof *s->threads, cmp_thread);
}

struct leak {
    uint64_t bytes;
    int node;
};

static int cmp_leak(const void *a, const void *b)
{
    const struct leak *x = a, *y = b;
    if (x->bytes != y->bytes)
        return x->bytes < y->bytes ? 1 : -1;
    return x->node - y->node;
}

size_t prof_session_leaks(struct prof_session *s, int *nodes, uint64_t *bytes, size_t max)
{
    /* Allocations that share a stack are reported once, with their total,
     * because a leaking call site matters more than a single block. */
    struct leak *found = calloc(s->live_used ? s->live_used : 1, sizeof *found);
    if (!found)
        return 0;
    size_t n = 0;
    for (size_t i = 0; i < s->live_cap; i++) {
        if (!s->live[i].addr)
            continue;
        size_t k = 0;
        for (; k < n; k++)
            if (found[k].node == s->live[i].node)
                break;
        if (k == n) {
            found[n].node = s->live[i].node;
            found[n].bytes = 0;
            n++;
        }
        found[k].bytes += s->live[i].bytes;
    }
    qsort(found, n, sizeof *found, cmp_leak);
    size_t written = n < max ? n : max;
    for (size_t i = 0; i < written; i++) {
        nodes[i] = found[i].node;
        bytes[i] = found[i].bytes;
    }
    free(found);
    return written;
}
