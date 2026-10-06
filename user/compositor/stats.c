/* Frame statistics (docs/design/graphics-performance.md): the counters
 * and the times of the frames since the last reset, in microseconds of
 * CLOCK_MONOTONIC. The debug interface sends them to compstat and to
 * x12settings, and the log receives a summary at exit.
 *
 * A frame is one composition of the damage. Its time runs from the start
 * of the composition to the end of the flush. The flush time is the part
 * spent in backend_present: the copy into the framebuffer, unless the
 * scene composes into it, and the flush ioctl. The compose time is the
 * rest of the frame.
 *
 * A latency runs from the earliest event that a frame presents to the end
 * of that frame. The events are the first damage after a frame, a commit
 * with a buffer, and a motion of the pointer. An event that causes no
 * composition is forgotten at the next idle timer expiration. */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/resource.h>
#include "comp.h"

/* Frame times in a histogram with 8 buckets per power of two: the values
 * 0 to 15 have a bucket each, and a larger value v with the highest bit e
 * falls into bucket 16 + (e - 4) * 8 + the three bits below bit e. */
#define HIST_BUCKETS 200

struct latency {
    uint64_t sum, max, count;
    long since;                         /* the earliest pending event, 0 for none */
};

static struct {
    long reset_at;                      /* microseconds */
    long cpu_at;                        /* CPU time of X12 at the reset, microseconds */
    uint64_t frames, rects, pixels, flushes, flush_rects, flush_bytes, wakeups, idle_timers;
    uint64_t compose_us, compose_max_us, flush_us, flush_max_us, frame_max_us;
    uint32_t hist[HIST_BUCKETS];
    struct latency latency[STATS_EVENTS];
} st;

static long frame_flush_us;             /* flush time of the frame being composed */
static uint64_t pool_bytes;             /* bytes of the mapped client pools */

static long cpu_us(void)
{
    struct rusage ru;
    if (getrusage(RUSAGE_SELF, &ru) < 0)
        return 0;
    return (long)(ru.ru_utime.tv_sec + ru.ru_stime.tv_sec) * 1000000 + ru.ru_utime.tv_usec + ru.ru_stime.tv_usec;
}

void stats_reset(void)
{
    memset(&st, 0, sizeof st);
    st.reset_at = uptime_us();
    st.cpu_at = cpu_us();
}

void stats_wakeup(void)
{
    st.wakeups++;
}

void stats_idle_timer(void)
{
    st.idle_timers++;
    for (int i = 0; i < STATS_EVENTS; i++)
        st.latency[i].since = 0;
}

void stats_mark(enum stats_event e)
{
    if (!st.latency[e].since)
        st.latency[e].since = uptime_us();
}

long stats_frame_begin(void)
{
    frame_flush_us = 0;
    return uptime_us();
}

void stats_rect(long device_pixels)
{
    st.rects++;
    st.pixels += (uint64_t)device_pixels;
}

void stats_flush(long rects, long bytes, long us)
{
    st.flushes++;
    st.flush_rects += (uint64_t)rects;
    st.flush_bytes += (uint64_t)bytes;
    frame_flush_us += us;
}

static int bucket_of(uint64_t v)
{
    if (v < 16)
        return (int)v;
    int e = 63 - __builtin_clzll(v);
    int b = 16 + (e - 4) * 8 + (int)((v >> (e - 3)) & 7);
    return b < HIST_BUCKETS ? b : HIST_BUCKETS - 1;
}

/* The middle of a bucket. */
static uint64_t bucket_value(int b)
{
    if (b < 16)
        return (uint64_t)b;
    int e = (b - 16) / 8 + 4, m = (b - 16) % 8;
    uint64_t low = (uint64_t)(8 + m) << (e - 3);
    return low + ((uint64_t)1 << (e - 3)) / 2;
}

long stats_frame_end(long t0)
{
    long now = uptime_us();
    uint64_t total = (uint64_t)(now - t0), flush = (uint64_t)frame_flush_us;
    uint64_t compose = total > flush ? total - flush : 0;
    st.frames++;
    st.compose_us += compose;
    st.flush_us += flush;
    if (compose > st.compose_max_us)
        st.compose_max_us = compose;
    if (flush > st.flush_max_us)
        st.flush_max_us = flush;
    if (total > st.frame_max_us)
        st.frame_max_us = total;
    st.hist[bucket_of(total)]++;
    for (int i = 0; i < STATS_EVENTS; i++) {
        struct latency *l = &st.latency[i];
        if (!l->since)
            continue;
        uint64_t v = (uint64_t)(now - l->since);
        l->sum += v;
        l->count++;
        if (v > l->max)
            l->max = v;
        l->since = 0;
    }
    return (long)total;
}

void stats_pool_mapped(long delta)
{
    pool_bytes += (uint64_t)delta;
}

/* The frame time below which percent of the frames lie. */
static uint64_t percentile(int percent)
{
    if (!st.frames)
        return 0;
    uint64_t want = (st.frames * (uint64_t)percent + 99) / 100, seen = 0;
    for (int b = 0; b < HIST_BUCKETS; b++) {
        seen += st.hist[b];
        if (seen >= want)
            return bucket_value(b);
    }
    return bucket_value(HIST_BUCKETS - 1);
}

static uint64_t average(const struct latency *l)
{
    return l->count ? l->sum / l->count : 0;
}

void stats_values(long *count, long *ms, long *max)
{
    *count = (long)st.frames;
    *ms = (long)((st.compose_us + st.flush_us) / 1000);
    *max = (long)(st.frame_max_us / 1000);
}

void stats_send(struct wire_resource *r)
{
    const struct latency *l = st.latency;
    const struct { const char *key; uint64_t value; } values[] = {
        { "elapsed_us", (uint64_t)(uptime_us() - st.reset_at) },
        { "frames", st.frames },
        { "rects", st.rects },
        { "pixels", st.pixels },
        { "flushes", st.flushes },
        { "flush_rects", st.flush_rects },
        { "flush_bytes", st.flush_bytes },
        { "compose_us", st.compose_us },
        { "compose_max_us", st.compose_max_us },
        { "flush_us", st.flush_us },
        { "flush_max_us", st.flush_max_us },
        { "frame_max_us", st.frame_max_us },
        { "frame_p50_us", percentile(50) },
        { "frame_p95_us", percentile(95) },
        { "frame_p99_us", percentile(99) },
        { "damage_latency_us", average(&l[STATS_DAMAGE]) },
        { "damage_latency_max_us", l[STATS_DAMAGE].max },
        { "commit_latency_us", average(&l[STATS_COMMIT]) },
        { "commit_latency_max_us", l[STATS_COMMIT].max },
        { "input_latency_us", average(&l[STATS_INPUT]) },
        { "input_latency_max_us", l[STATS_INPUT].max },
        { "wakeups", st.wakeups },
        { "idle_timers", st.idle_timers },
        { "cpu_us", (uint64_t)(cpu_us() - st.cpu_at) },
        { "back_bytes", (uint64_t)backend_buffer_bytes() },
        { "pool_bytes", pool_bytes },
    };
    for (size_t i = 0; i < sizeof values / sizeof values[0]; i++)
        debug_send_frame_stat(r, values[i].key, (uint32_t)(values[i].value >> 32), (uint32_t)values[i].value);
    debug_send_frame_stats_done(r);
}

void stats_log(void)
{
    comp_log("frame stats: %llu frames, %llu us compose, %llu us flush, %llu us max, p50 %llu us, p95 %llu us",
             (unsigned long long)st.frames, (unsigned long long)st.compose_us, (unsigned long long)st.flush_us,
             (unsigned long long)st.frame_max_us, (unsigned long long)percentile(50),
             (unsigned long long)percentile(95));
}
