/* Queries and portable exports of an aggregated capture. No device access.
 * Store weights as integers on disk so sub-millisecond work is not rounded. */
#include <prof/profile.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

static int by_cost(const void *a, const void *b)
{
    const struct prof_breakdown *x = a, *y = b;
    if (x->total != y->total)
        return x->total > y->total ? -1 : 1;
    return (x->node > y->node) - (x->node < y->node);
}

size_t prof_tree_breakdown(const struct prof_tree *t, int root,
                           struct prof_breakdown *out, size_t max)
{
    if (!t || root < 0 || root >= t->count || !out || !max)
        return 0;
    const struct prof_node *n = &t->nodes[root];
    uint64_t child_extra = 0;
    size_t used = 1;
    for (int c = n->child; c >= 0; c = t->nodes[c].sibling) {
        const struct prof_node *child = &t->nodes[c];
        child_extra += child->extra;
        if (used < max)
            out[used++] = (struct prof_breakdown){c, child->total, child->self, child->extra};
    }
    out[0] = (struct prof_breakdown){root, n->self, n->self, n->extra - child_extra};
    qsort(out, used, sizeof *out, by_cost);
    return used;
}

uint64_t prof_tree_match_weight(const struct prof_tree *t, int root, const char *query)
{
    if (!t || root < 0 || root >= t->count || !query || !*query)
        return 0;
    uint64_t weight = 0;
    /* Charge exclusive weights only. Walking ancestors also establishes
     * membership in the requested subtree without relying on node order. */
    for (int i = root; i < t->count; i++) {
        if (!t->nodes[i].self)
            continue;
        int match = 0;
        for (int p = i; p >= 0; p = t->nodes[p].parent) {
            if (p && strstr(prof_names_get(t->names, t->nodes[p].name), query))
                match = 1;
            if (p == root) {
                if (match)
                    weight += t->nodes[i].self;
                break;
            }
        }
    }
    return weight;
}

static void json_string(FILE *f, const char *s)
{
    fputc('"', f);
    for (const unsigned char *p = (const unsigned char *)(s ? s : ""); *p; p++) {
        if (*p == '"' || *p == '\\') {
            fputc('\\', f);
            fputc(*p, f);
        } else if (*p < 32 || *p >= 127) {
            /* ELF symbol names are bytes, not necessarily valid UTF-8. */
            fprintf(f, "\\u%04x", *p);
        } else {
            fputc(*p, f);
        }
    }
    fputc('"', f);
}

static void write_json(FILE *f, const struct prof_session *s, const struct prof_stats *st)
{
    static const char *const names[] = {"cpu", "offcpu", "heap", "io"};
    fprintf(f, "{\"format\":\"minios-profile\",\"version\":1,\"events\":%llu,"
               "\"period_ns\":%llu,\"first_ns\":%llu,\"last_ns\":%llu,\"dropped\":",
            (unsigned long long)s->events, (unsigned long long)s->period_ns,
            (unsigned long long)s->first_ns, (unsigned long long)s->last_ns);
    if (st)
        fprintf(f, "%llu", (unsigned long long)st->dropped);
    else
        fputs("null", f);
    fprintf(f, ",\"live_bytes\":%llu,\"live_peak\":%llu,\"alloc_bytes\":%llu,"
               "\"freed_bytes\":%llu,\"counts\":[",
            (unsigned long long)s->live_bytes, (unsigned long long)s->live_peak,
            (unsigned long long)s->alloc_bytes, (unsigned long long)s->freed_bytes);
    for (int i = 0; i < PROF_EV_TYPES; i++)
        fprintf(f, "%s%llu", i ? "," : "", (unsigned long long)s->counts[i]);
    fputs("],\"views\":[", f);
    for (int v = 0; v < PROF_VIEW_COUNT; v++) {
        const struct prof_tree *t = &s->view[v];
        fprintf(f, "%s{\"name\":\"%s\",\"unit\":\"%s\",\"extra_unit\":\"%s\",\"nodes\":[",
                v ? "," : "", names[v], v == PROF_VIEW_HEAP ? "bytes" : "nanoseconds",
                v == PROF_VIEW_HEAP || v == PROF_VIEW_IO ? "bytes" : "none");
        for (int i = 0; i < t->count; i++) {
            const struct prof_node *n = &t->nodes[i];
            fprintf(f, "%s{\"id\":%d,\"parent\":%d,\"name\":", i ? "," : "", i, n->parent);
            json_string(f, i ? prof_names_get(t->names, n->name) : "all");
            fprintf(f, ",\"kernel\":%s,\"total\":%llu,\"self\":%llu,\"extra\":%llu,\"self_events\":%u}",
                    n->kernel ? "true" : "false", (unsigned long long)n->total,
                    (unsigned long long)n->self, (unsigned long long)n->extra, n->count);
        }
        fputs("]}", f);
    }
    fputs("],\"threads\":[", f);
    for (size_t i = 0; i < s->nthreads; i++) {
        const struct prof_thread_stat *t = &s->threads[i];
        fprintf(f, "%s{\"pid\":%u,\"tid\":%u,\"name\":", i ? "," : "", t->pid, t->tid);
        json_string(f, t->name);
#define FIELD(key) fprintf(f, ",\"" #key "\":%llu", (unsigned long long)t->key)
        FIELD(on_cpu_ns); FIELD(off_cpu_ns); FIELD(ready_ns); FIELD(samples);
        FIELD(blocks); FIELD(preempts); FIELD(allocs); FIELD(alloc_bytes);
        FIELD(ios); FIELD(io_ns); FIELD(io_bytes);
#undef FIELD
        fputc('}', f);
    }
    fputs("]}\n", f);
}

static void write_folded(FILE *f, const struct prof_tree *t)
{
    for (int i = 0; i < t->count; i++) {
        if (!t->nodes[i].self)
            continue;
        int path[PROF_MAX_FRAMES], depth = 0;
        for (int p = i; p > 0 && depth < PROF_MAX_FRAMES; p = t->nodes[p].parent)
            path[depth++] = p;
        if (!depth)
            fputs("[unattributed]", f);
        for (int d = depth - 1; d >= 0; d--) {
            const struct prof_node *n = &t->nodes[path[d]];
            if (d != depth - 1)
                fputc(';', f);
            const unsigned char *p = (const unsigned char *)prof_names_get(t->names, n->name);
            /* Folded text has no escaping convention. Replace delimiters
             * and control bytes so names cannot inject frames or records. */
            for (; *p; p++)
                fputc(*p == ';' || *p <= 32 || *p == 127 ? '_' : *p, f);
            if (n->kernel)
                fputs("_[k]", f);
        }
        fprintf(f, " %llu\n", (unsigned long long)t->nodes[i].self);
    }
}

int prof_session_export(const struct prof_session *s, const struct prof_stats *st,
                        const char *path, int format, int view)
{
    if (!s || !path || !*path || view < 0 || view >= PROF_VIEW_COUNT ||
        (format != PROF_EXPORT_JSON && format != PROF_EXPORT_FOLDED)) {
        errno = EINVAL;
        return -1;
    }
    size_t len = strlen(path) + 48;
    char *tmp = malloc(len);
    if (!tmp) {
        errno = ENOMEM;
        return -1;
    }
    snprintf(tmp, len, "%s.tmp.%d", path, getpid());
    int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) {
        free(tmp);
        return -1;
    }
    FILE *f = fdopen(fd, "w");
    int error = 0;
    if (!f) {
        error = errno;
        close(fd);
    } else {
        if (format == PROF_EXPORT_JSON)
            write_json(f, s, st);
        else
            write_folded(f, &s->view[view]);
        if (ferror(f))
            error = errno ? errno : EIO;
        if (fclose(f) < 0 && !error)
            error = errno ? errno : EIO;
    }
    if (!error && rename(tmp, path) < 0)
        error = errno;
    if (error)
        unlink(tmp);
    free(tmp);
    if (error)
        errno = error;
    return error ? -1 : 0;
}
