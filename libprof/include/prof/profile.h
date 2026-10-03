#pragma once
/* Profiler client (M48): control of /dev/profile, the event stream,
 * symbol resolution for the kernel and for user processes, and the
 * aggregation the front ends display: call trees, flame graph layout,
 * flat histograms, live allocations and per thread statistics.
 *
 * A front end opens the device, configures the event classes it wants,
 * feeds every record to a session, and reads the results out of the
 * session. See docs/design/profile.md. */
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <minios/abi.h>

/* ---- /dev/profile ---- */
int prof_open(void);                        /* descriptor or -1 */
int prof_start(int fd, pid_t pid);          /* pid 0: every process */
int prof_stop(int fd);
int prof_set_divider(int fd, unsigned ticks);
int prof_configure(int fd, const struct prof_config *cfg);
int prof_get_stats(int fd, struct prof_stats *st);
/* Read whole events into buf; returns the bytes read, 0 when none are
 * pending. Records are delivered in time order. */
ssize_t prof_read_events(int fd, void *buf, size_t bytes);
/* Iterate the records of such a buffer: NULL ends the walk. */
const struct prof_event *prof_event_first(const void *buf, size_t bytes);
const struct prof_event *prof_event_next(const void *buf, size_t bytes,
                                         const struct prof_event *e);

/* ---- symbol tables ---- */
struct prof_sym {
    uint64_t addr;
    uint64_t size;
    const char *name;
};
/* A shared library mapped into the process the table describes: the
 * addresses [start, end) contain the file from offset on, and the library's
 * own table gives the symbols by link address. */
struct prof_module {
    uint64_t start, end, offset;
    struct prof_symtab *syms;
    char *path;
    int owner;                  /* this entry frees syms and path */
};
struct prof_symtab {
    struct prof_sym *syms;      /* sorted by address */
    size_t count;
    char *strings;
    struct prof_module *modules;
    size_t nmodules;
};
/* The function symbols of an ELF file, or NULL. */
struct prof_symtab *prof_symtab_load_elf(const char *path);
/* Attach the shared libraries of process pid, read from /dev/maps, to t so
 * that lookups resolve addresses inside them. Returns the number added. */
int prof_symtab_add_maps(struct prof_symtab *t, pid_t pid);
/* The kernel's symbols from /dev/ksyms, or NULL. */
struct prof_symtab *prof_symtab_load_kernel(void);
void prof_symtab_free(struct prof_symtab *t);
/* The symbol containing addr, or NULL; *off receives the offset inside. */
const struct prof_sym *prof_symtab_lookup(const struct prof_symtab *t, uint64_t addr, uint64_t *off);

/* ---- resolver: one symbol table per process, plus the kernel ---- */
struct prof_resolver;
struct prof_resolver *prof_resolver_new(void);
void prof_resolver_free(struct prof_resolver *r);
/* Load the kernel table; without it kernel addresses remain numeric. */
int prof_resolver_kernel(struct prof_resolver *r);
/* The command name of a pid, read from /dev/proc and cached, or NULL. */
const char *prof_resolver_procname(struct prof_resolver *r, pid_t pid);
/* Name of an address: "symbol", or "0x..." when it cannot be resolved.
 * The returned string remains valid for the life of the resolver. */
const char *prof_resolve(struct prof_resolver *r, pid_t pid, int kernel, uint64_t addr);

/* ---- interned names ---- */
struct prof_names;
struct prof_names *prof_names_new(void);
void prof_names_free(struct prof_names *n);
/* The id of s, interning it when new. Id 0 is the empty name. */
int prof_names_intern(struct prof_names *n, const char *s);
const char *prof_names_get(const struct prof_names *n, int id);

/* ---- call tree ---- */
/* Weights depend on the view: nanoseconds for CPU, off CPU and IO time,
 * bytes for allocations. extra carries the second magnitude of a view:
 * bytes for IO, bytes still allocated for the heap. Both are accumulated
 * along the whole path, so a node contains the totals of its subtree. */
struct prof_node {
    int name;                   /* id in the tree's name table */
    int parent, child, sibling;
    int depth;
    uint64_t total, extra;
    uint64_t self;              /* weight of events that ended here */
    uint32_t count;
    uint8_t kernel;
};
struct prof_tree {
    struct prof_node *nodes;    /* node 0 is the root */
    int count, cap;
    struct prof_names *names;   /* borrowed, not freed with the tree */
};
/* Frames are given outermost first, as a flame graph reads them. */
struct prof_stack {
    int names[PROF_MAX_FRAMES];
    uint8_t kernel[PROF_MAX_FRAMES];
    int count;
};
int prof_tree_init(struct prof_tree *t, struct prof_names *names);
void prof_tree_clear(struct prof_tree *t);
void prof_tree_free(struct prof_tree *t);
/* Add one stack; returns the index of its leaf, or -1. */
int prof_tree_add(struct prof_tree *t, const struct prof_stack *s, uint64_t weight, uint64_t extra);
/* Add weight to a leaf and to every node above it; weights may be negative
 * so an allocation can be retired when it is freed. */
void prof_tree_charge(struct prof_tree *t, int leaf, int64_t weight, int64_t extra);
/* Order every node's children by name, so a redrawn view is stable. */
void prof_tree_sort(struct prof_tree *t);
/* The full path of a node, outermost first, into names[]; returns the
 * number of frames written. */
int prof_tree_path(const struct prof_tree *t, int node, const char **names, int max);

/* ---- flame graph ---- */
struct prof_flame_box {
    int node;
    int depth;
    uint64_t start, width;      /* on a scale of the root's weight */
};
/* Lay the subtree under root into boxes, left to right by child order.
 * With use_extra the second magnitude gives the widths. Returns the boxes
 * written, which is at most max. Call prof_tree_sort first. */
size_t prof_flame_layout(const struct prof_tree *t, int root, int use_extra,
                         struct prof_flame_box *out, size_t max);

/* Direct callees plus a self row for one call site, sorted by cost.
 * Allocate t->count entries to avoid truncation. The self row has node=root.
 * extra is exclusive for that row and inclusive for each callee. */
struct prof_breakdown {
    int node;
    uint64_t total, self, extra;
};
size_t prof_tree_breakdown(const struct prof_tree *t, int root,
                           struct prof_breakdown *out, size_t max);
/* Case sensitive substring search. Count each matching stack's weight
 * once, even when several ancestors match (including recursion). */
uint64_t prof_tree_match_weight(const struct prof_tree *t, int root, const char *query);

/* ---- histogram ---- */
struct prof_bucket {
    char *key;                  /* symbol name or a chain "a;b;c" */
    unsigned count;
    unsigned kernel;            /* samples of the key taken in kernel mode */
    uint64_t weight;            /* nanoseconds or bytes, see the view */
    uint64_t extra;
};
struct prof_hist {
    struct prof_bucket *buckets;
    size_t count, cap;
    unsigned total;
    uint64_t weight;            /* sum of the bucket weights */
    /* Keys are found through an open addressing index of bucket numbers,
     * rebuilt after a sort has moved the buckets around. */
    int *index;
    size_t index_cap;
    int index_stale;
};
void prof_hist_init(struct prof_hist *h);
void prof_hist_clear(struct prof_hist *h);
void prof_hist_add(struct prof_hist *h, const char *key, int kernel);
void prof_hist_add_weight(struct prof_hist *h, const char *key, int kernel,
                          uint64_t weight, uint64_t extra);
/* Sort buckets by descending weight, then count. */
void prof_hist_sort(struct prof_hist *h);

/* ---- session ---- */
/* The views a session maintains. Each has a call tree and a flat table of
 * the frame the event ended in. */
enum {
    PROF_VIEW_CPU,              /* on CPU time, nanoseconds */
    PROF_VIEW_OFFCPU,           /* time blocked or waiting to run */
    PROF_VIEW_HEAP,             /* kernel heap bytes; extra is still live */
    PROF_VIEW_IO,               /* transfer latency; extra is bytes */
    PROF_VIEW_COUNT,
};

/* on_cpu_ns, off_cpu_ns and ready_ns are measured by the kernel and are
 * only filled while the scheduler class is recorded; samples counts timer
 * samples, which estimate CPU time on its own when it is not. */
struct prof_thread_stat {
    uint32_t pid, tid;
    uint64_t on_cpu_ns, off_cpu_ns, ready_ns;
    uint64_t samples, blocks, preempts, allocs, alloc_bytes, ios, io_ns, io_bytes;
    const char *name;           /* process name, owned by the resolver */
};

struct prof_session {
    struct prof_names *names;
    struct prof_resolver *res;
    struct prof_tree view[PROF_VIEW_COUNT];
    struct prof_hist flat[PROF_VIEW_COUNT];
    struct prof_thread_stat *threads;
    size_t nthreads, threads_cap;
    uint64_t period_ns;         /* CPU time one sample stands for */
    uint64_t events, first_ns, last_ns;
    uint64_t counts[PROF_EV_TYPES];
    uint64_t live_bytes, live_peak, alloc_bytes, freed_bytes;
    size_t live_count;          /* allocations without a matching free */
    pid_t exclude;              /* events of this process are ignored */
    int locked_samples;         /* kernel samples taken with interrupts off */
    struct prof_live *live;     /* address to allocation map */
    size_t live_cap, live_used;
    struct prof_pending *pending; /* threads between block and run */
    size_t pending_cap;
};

struct prof_session *prof_session_new(struct prof_resolver *res, uint64_t period_ns);
void prof_session_free(struct prof_session *s);
void prof_session_reset(struct prof_session *s);
/* Feed one record. Returns 0 when it was counted, -1 when it was ignored. */
int prof_session_event(struct prof_session *s, const struct prof_event *e);
/* Feed a whole read buffer; returns the number of records consumed. */
size_t prof_session_feed(struct prof_session *s, const void *buf, size_t bytes);
/* Thread statistics sorted by descending on CPU time. */
void prof_session_sort_threads(struct prof_session *s);
/* Allocations with no matching free, grouped by the stack that made them
 * and ordered by descending bytes. Returns the number of entries written;
 * each is a node of the heap view's tree. */
size_t prof_session_leaks(struct prof_session *s, int *nodes, uint64_t *bytes, size_t max);

enum { PROF_EXPORT_JSON, PROF_EXPORT_FOLDED };
/* Atomically replace path after writing and closing a temporary file.
 * JSON contains all views and thread statistics. Folded stacks contain
 * the selected view's self weights in ns, or allocated bytes for heap.
 * stats may be NULL. Returns -1 with errno on failure. */
int prof_session_export(const struct prof_session *s, const struct prof_stats *stats,
                        const char *path, int format, int view);

/* ---- attribution helpers ---- */
/* Format an address through user and kernel tables: "name+0xoff" or the
 * hexadecimal address. */
void prof_format_addr(const struct prof_symtab *user, const struct prof_symtab *kernel, int is_kernel,
                      uint64_t addr, int with_offset, char *buf, size_t size);
/* True when name is one of the lock primitives the timer interrupt lands
 * in when it was pending while interrupts were disabled. */
int prof_is_lock_primitive(const char *name);
/* The frame an event should be charged to: the innermost frame that is not
 * a lock primitive. *locked is set when primitives were skipped. */
const char *prof_session_leaf(struct prof_session *s, const struct prof_event *e, int *locked);
