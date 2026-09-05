#include <tests/ktest.h>
#include <sync/atomic.h>
#include <sync/percpu.h>
#include <sync/mpsc.h>
#include <sync/rcu.h>
#include <sync/ring.h>
#include <sched/thread.h>
#include <sched/sched.h>
#include <drivers/timer.h>
#include <ipc/pipe.h>
#include <ipc/poll.h>
#include <fs/vfs.h>
#include <console.h>

#define LF_WORKERS 8
#define LF_ITERS   20000

struct lf_worker {
    struct mpsc_node node;
    int id;
};

static atomic_u64_t lf_total = ATOMIC_U64_INIT(0);
static struct percpu_counter lf_percpu;
static struct mpsc_head lf_done;

static void lf_worker(void *arg)
{
    struct lf_worker *w = arg;
    for (int i = 0; i < LF_ITERS; i++) {
        atomic_u64_fetch_add_relaxed(&lf_total, 1);
        percpu_counter_inc(&lf_percpu);
        if ((i & 255) == 0)
            sched_preempt();
    }
    mpsc_push(&lf_done, &w->node);
}

static bool rcu_called;
static void test_rcu_callback(struct rcu_head *head)
{
    ktest_assert(read_rflags() & RFLAGS_IF, "RCU callback ran with interrupts disabled");
    /* VMA reclamation may close the last file reference and sleep in a
     * filesystem transaction. Exercise that callback contract directly. */
    sleep_ms(2);
    __atomic_store_n(&rcu_called, true, __ATOMIC_RELEASE);
}

static void test_lockfree(void)
{
    atomic_u64_store_relaxed(&lf_total, 0);
    percpu_counter_init(&lf_percpu, 0);
    mpsc_init(&lf_done);
    struct lf_worker args[LF_WORKERS];
    struct thread *threads[LF_WORKERS];
    for (int i = 0; i < LF_WORKERS; i++) {
        args[i].id = i;
        threads[i] = thread_create("lfworker", lf_worker, &args[i], 0);
        ktest_assert(threads[i], "lockfree worker %d", i);
    }
    for (int i = 0; i < LF_WORKERS; i++)
        thread_join(threads[i]);
    uint64_t expected = LF_WORKERS * LF_ITERS;
    ktest_assert(atomic_u64_load_acquire(&lf_total) == expected,
                 "atomic total %lu", atomic_u64_load_relaxed(&lf_total));
    ktest_assert((uint64_t)percpu_counter_sum(&lf_percpu) == expected,
                 "percpu total %ld", percpu_counter_sum(&lf_percpu));

    int nodes = 0;
    struct mpsc_node *node = mpsc_take_all(&lf_done);
    while (node) {
        nodes++;
        node = node->next;
    }
    ktest_assert(nodes == LF_WORKERS, "mpsc nodes %d", nodes);

    refcount_t refs = REFCOUNT_INIT(1);
    ktest_assert(refcount_inc_not_zero(&refs), "ref acquire");
    ktest_assert(!refcount_dec_and_test(&refs), "ref premature zero");
    ktest_assert(refcount_dec_and_test(&refs), "ref final release");
    ktest_assert(!refcount_inc_not_zero(&refs), "resurrected zero ref");

    struct rcu_head head;
    rcu_called = false;
    rcu_read_lock();
    ktest_assert(rcu_read_held(), "rcu read state");
    rcu_read_unlock();
    rcu_call(&head, test_rcu_callback);
    uint64_t deadline = timer_ms() + 1000;
    while (!__atomic_load_n(&rcu_called, __ATOMIC_ACQUIRE) && timer_ms() < deadline)
        sleep_ms(1);
    ktest_assert(rcu_called, "rcu callback timeout");
    kprintf("lockfree: %lu atomic/percpu updates, %d mpsc nodes\n", expected, nodes);
}
KTEST_DEFINE("lockfree", test_lockfree);

#define TEST_RING_SIZE 64
#define TEST_RING_BYTES 20000
static uint8_t test_ring_data[TEST_RING_SIZE];
static struct spsc_ring test_ring;
static bool ring_producer_done;

static void ring_producer(void *arg)
{
    for (unsigned i = 0; i < TEST_RING_BYTES;) {
        uint8_t byte = (uint8_t)i;
        if (ring_write(&test_ring, &byte, 1) == 1)
            i++;
        else
            sched_yield();
    }
    __atomic_store_n(&ring_producer_done, true, __ATOMIC_RELEASE);
}

static void test_ring_spsc(void)
{
    ring_init(&test_ring, test_ring_data, sizeof test_ring_data);
    uint8_t in[50], out[50];
    for (unsigned i = 0; i < sizeof in; i++)
        in[i] = (uint8_t)i;
    ktest_assert(ring_write(&test_ring, in, 50) == 50, "ring initial write");
    ktest_assert(ring_read(&test_ring, out, 37) == 37, "ring initial read");
    ktest_assert(ring_write(&test_ring, in, 50) == 50, "ring wrapped write");
    ktest_assert(ring_count(&test_ring) == 63, "ring wrapped count %lu", ring_count(&test_ring));
    ring_init(&test_ring, test_ring_data, sizeof test_ring_data);

    ring_producer_done = false;
    struct thread *producer = thread_create("ringprod", ring_producer, NULL, 0);
    ktest_assert(producer, "ring producer");
    for (unsigned expected = 0; expected < TEST_RING_BYTES;) {
        uint8_t byte;
        if (ring_read(&test_ring, &byte, 1) == 0) {
            sched_yield();
            continue;
        }
        ktest_assert(byte == (uint8_t)expected, "ring byte %u at %u", byte, expected);
        expected++;
    }
    thread_join(producer);
    ktest_assert(ring_producer_done && ring_count(&test_ring) == 0, "ring completion");
    kprintf("ring: %d ordered bytes across wraparound\n", TEST_RING_BYTES);
}
KTEST_DEFINE("ring", test_ring_spsc);

struct poll_writer_arg {
    struct file *file;
};

static void poll_writer(void *arg)
{
    struct poll_writer_arg *a = arg;
    sleep_ms(20);
    uint64_t pos = 0;
    char byte = 'P';
    a->file->ops->write(a->file, &byte, 1, &pos);
}

static void test_poll_wake(void)
{
    struct file *rd, *wr;
    ktest_assert(pipe_create(&rd, &wr) == 0, "poll pipe create");
    struct poll_writer_arg arg = { .file = wr };
    struct thread *writer = thread_create("pollwriter", poll_writer, &arg, 0);
    ktest_assert(writer, "poll writer");
    struct file *files[1] = { rd };
    struct pollfd pfd = { .fd = 0, .events = POLLIN };
    uint64_t start = timer_ms();
    long ready = poll_files(files, &pfd, 1, 1000);
    uint64_t elapsed = timer_ms() - start;
    ktest_assert(ready == 1 && (pfd.revents & POLLIN), "poll ready %ld events %x", ready, pfd.revents);
    ktest_assert(elapsed >= 10 && elapsed < 500, "poll elapsed %lu", elapsed);
    thread_join(writer);
    file_put(rd);
    file_put(wr);
    kprintf("poll_wake: object-local wake after %lu ms\n", elapsed);
}
KTEST_DEFINE("poll_wake", test_poll_wake);
