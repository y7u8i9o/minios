/* M35 test: pthreads, futex based synchronization, thread local errno
 * and keys, the thread safety of malloc and stdio. Exits 0 on success. */
#include <pthread.h>
#include <sys/thread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("pthreadtest: FAIL " __VA_ARGS__); printf("\n"); } } while (0)

#define WORKERS 8
#define ROUNDS 2000

static pthread_mutex_t counter_lock = PTHREAD_MUTEX_INITIALIZER;
static long counter;

static void raw_entry(void *arg) { thread_exit(0); }

/* A C thread entry must preserve the ABI alignment used by SIMD spills. */
static void *aligned_stack_worker(void *arg)
{
    float values[4] __attribute__((aligned(16)));
#if defined(__x86_64__)
    /* movaps faults on a misaligned address. */
    __asm__ volatile("xorps %%xmm0, %%xmm0\n\tmovaps %%xmm0, %0"
                     : "=m"(values) : : "xmm0");
#elif defined(__aarch64__)
    /* An sp that is not 16 byte aligned faults at any sp based access. */
    __asm__ volatile("movi v0.4s, #0\n\tstr q0, %0"
                     : "=m"(values) : : "v0");
#endif
    return (void *)(long)(values[0] == 0 && values[3] == 0);
}

/* Opening/closing private streams must coexist with flushing the registry. */
static void *file_worker(void *arg)
{
    char path[80];
    snprintf(path, sizeof path, "/tmp/pthread-file-%d", gettid());
    for (int i = 0; i < 100; i++) {
        FILE *f = fopen(path, "w");
        if (!f) return NULL;
        fputs("thread data\n", f);
        fflush(NULL);
        if (fclose(f) != 0) return NULL;
    }
    unlink(path);
    return (void *)1;
}

static void *count_worker(void *arg)
{
    for (int i = 0; i < ROUNDS; i++) {
        pthread_mutex_lock(&counter_lock);
        counter++;
        pthread_mutex_unlock(&counter_lock);
    }
    return (void *)(long)(counter > 0);
}

/* A bounded queue between producers and consumers. */
#define QUEUE_SIZE 4
static pthread_mutex_t queue_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t not_empty = PTHREAD_COND_INITIALIZER, not_full = PTHREAD_COND_INITIALIZER;
static int queue[QUEUE_SIZE], qhead, qcount;
static long consumed_sum;
#define ITEMS 500

static void *producer(void *arg)
{
    int base = (int)(long)arg;
    for (int i = 0; i < ITEMS; i++) {
        pthread_mutex_lock(&queue_lock);
        while (qcount == QUEUE_SIZE)
            pthread_cond_wait(&not_full, &queue_lock);
        queue[(qhead + qcount) % QUEUE_SIZE] = base + i;
        qcount++;
        pthread_cond_signal(&not_empty);
        pthread_mutex_unlock(&queue_lock);
    }
    return NULL;
}

static void *consumer(void *arg)
{
    long sum = 0;
    for (int i = 0; i < ITEMS; i++) {
        pthread_mutex_lock(&queue_lock);
        while (qcount == 0)
            pthread_cond_wait(&not_empty, &queue_lock);
        sum += queue[qhead];
        qhead = (qhead + 1) % QUEUE_SIZE;
        qcount--;
        pthread_cond_signal(&not_full);
        pthread_mutex_unlock(&queue_lock);
    }
    pthread_mutex_lock(&queue_lock);
    consumed_sum += sum;
    pthread_mutex_unlock(&queue_lock);
    return NULL;
}

/* errno is per thread: a failing open in one thread does not disturb
 * another thread's errno. */
static pthread_mutex_t errno_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t errno_cond = PTHREAD_COND_INITIALIZER;
static int errno_stage;

static void *errno_worker(void *arg)
{
    errno = 0;
    CHECK(open("/nonexistent/file", 0) < 0 && errno == ENOENT, "worker open fails with ENOENT");
    pthread_mutex_lock(&errno_lock);
    errno_stage = 1;
    pthread_cond_broadcast(&errno_cond);
    while (errno_stage != 2)
        pthread_cond_wait(&errno_cond, &errno_lock);
    pthread_mutex_unlock(&errno_lock);
    CHECK(errno == ENOENT, "worker errno retained %d", errno);
    return NULL;
}

static pthread_key_t key;
static int destructor_runs;

static void key_destructor(void *value)
{
    __atomic_fetch_add(&destructor_runs, (int)(long)value, __ATOMIC_RELAXED);
}

static void *key_worker(void *arg)
{
    CHECK(pthread_getspecific(key) == NULL, "key starts NULL in a new thread");
    pthread_setspecific(key, arg);
    usleep(1000);
    CHECK(pthread_getspecific(key) == arg, "key value per thread");
    return NULL;
}

static pthread_once_t once = PTHREAD_ONCE_INIT;
static int once_runs;

static void once_init(void)
{
    once_runs++;
}

static void *once_worker(void *arg)
{
    pthread_once(&once, once_init);
    return NULL;
}

static void *detached_worker(void *arg)
{
    __atomic_add_fetch((int *)arg, 1, __ATOMIC_SEQ_CST);
    return NULL;
}

static void *malloc_worker(void *arg)
{
    void *blocks[64];
    for (int round = 0; round < 50; round++) {
        for (int i = 0; i < 64; i++) {
            blocks[i] = malloc((size_t)(i * 13 + 1));
            memset(blocks[i], (int)(long)arg, (size_t)(i * 13 + 1));
        }
        for (int i = 0; i < 64; i++) {
            CHECK(((unsigned char *)blocks[i])[0] == (unsigned char)(long)arg, "block content");
            free(blocks[i]);
        }
    }
    return NULL;
}

static void *print_worker(void *arg)
{
    for (int i = 0; i < 20; i++)
        fprintf(stdout, "pthreadtest: line from thread %d number %d\n", (int)(long)arg, i);
    return NULL;
}

static void *rw_reader(void *arg)
{
    pthread_rwlock_t *l = arg;
    for (int i = 0; i < 100; i++) {
        pthread_rwlock_rdlock(l);
        CHECK(counter >= 0, "reader sees a consistent value");
        pthread_rwlock_unlock(l);
    }
    return NULL;
}

static void *rw_writer(void *arg)
{
    pthread_rwlock_t *l = arg;
    for (int i = 0; i < 100; i++) {
        pthread_rwlock_wrlock(l);
        counter = -1;
        counter = 1;
        pthread_rwlock_unlock(l);
    }
    return NULL;
}

int main(void)
{
    pthread_t t[WORKERS];
    CHECK(pthread_self() != NULL && pthread_equal(pthread_self(), pthread_self()), "pthread_self");
    unsigned char tiny[32] __attribute__((aligned(16)));
    memset(tiny, 0x5a, sizeof tiny);
    thread_t raw;
    CHECK(thread_create(&raw, raw_entry, NULL, tiny + 15, 16) == -1 && errno == EINVAL,
          "reject stack without aligned entry slot");
    CHECK(tiny[8] == 0x5a && tiny[14] == 0x5a, "rejected stack leaves surrounding bytes intact");

    /* Mutex contention. */
    for (int i = 0; i < WORKERS; i++)
        CHECK(pthread_create(&t[i], NULL, count_worker, NULL) == 0, "create worker %d", i);
    for (int i = 0; i < WORKERS; i++) {
        void *result = NULL;
        CHECK(pthread_join(t[i], &result) == 0 && result == (void *)1, "join worker %d", i);
    }
    CHECK(counter == (long)WORKERS * ROUNDS, "counter %ld", counter);
    CHECK(counter_lock.state == 0, "mutex released");

    /* Condition variables: two producers, two consumers. */
    pthread_t p[2], c[2];
    for (int i = 0; i < 2; i++) {
        CHECK(pthread_create(&c[i], NULL, consumer, NULL) == 0, "create consumer");
        CHECK(pthread_create(&p[i], NULL, producer, (void *)(long)(i * 10000)) == 0, "create producer");
    }
    for (int i = 0; i < 2; i++) {
        pthread_join(p[i], NULL);
        pthread_join(c[i], NULL);
    }
    long expected = 0;
    for (int i = 0; i < ITEMS; i++)
        expected += i + (10000 + i);
    CHECK(consumed_sum == expected, "consumed sum %ld, expected %ld", consumed_sum, expected);

    /* Recursive and error checking mutexes. */
    pthread_mutexattr_t attr;
    pthread_mutex_t rec, chk;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&rec, &attr);
    CHECK(pthread_mutex_lock(&rec) == 0 && pthread_mutex_lock(&rec) == 0, "recursive lock twice");
    CHECK(pthread_mutex_unlock(&rec) == 0 && pthread_mutex_unlock(&rec) == 0 && rec.state == 0, "recursive unlock twice");
    CHECK(pthread_mutex_unlock(&rec) == EPERM, "unlock of a free recursive mutex");
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_ERRORCHECK);
    pthread_mutex_init(&chk, &attr);
    CHECK(pthread_mutex_lock(&chk) == 0 && pthread_mutex_lock(&chk) == EDEADLK, "error checking mutex");
    CHECK(pthread_mutex_trylock(&chk) == EBUSY, "trylock of a locked mutex");
    pthread_mutex_unlock(&chk);
    CHECK(pthread_mutex_trylock(&chk) == 0, "trylock of a free mutex");
    pthread_mutex_unlock(&chk);

    /* A timed wait that expires. */
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_nsec += 50000000;
    if (deadline.tv_nsec >= 1000000000) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000;
    }
    pthread_cond_t lonely = PTHREAD_COND_INITIALIZER;
    pthread_mutex_t lonely_lock = PTHREAD_MUTEX_INITIALIZER;
    long before = uptime_ms();
    pthread_mutex_lock(&lonely_lock);
    CHECK(pthread_cond_timedwait(&lonely, &lonely_lock, &deadline) == ETIMEDOUT, "timed wait expires");
    pthread_mutex_unlock(&lonely_lock);
    long waited = uptime_ms() - before;
    CHECK(waited >= 45 && waited < 500, "timed wait lasted %ld ms", waited);
    CHECK(lonely_lock.state == 0 && lonely.waiters == 0, "timed wait cleaned up");

    /* errno per thread. */
    pthread_t ew;
    errno = 0;
    CHECK(pthread_create(&ew, NULL, errno_worker, NULL) == 0, "create errno worker");
    pthread_mutex_lock(&errno_lock);
    while (errno_stage != 1)
        pthread_cond_wait(&errno_cond, &errno_lock);
    CHECK(errno == 0, "main errno untouched by the worker: %d", errno);
    errno = EINVAL;
    errno_stage = 2;
    pthread_cond_broadcast(&errno_cond);
    pthread_mutex_unlock(&errno_lock);
    pthread_join(ew, NULL);
    CHECK(errno == EINVAL, "main errno retained");

    /* Keys and destructors. */
    CHECK(pthread_key_create(&key, key_destructor) == 0, "key create");
    pthread_setspecific(key, (void *)100);
    pthread_t kw[3];
    for (int i = 0; i < 3; i++)
        pthread_create(&kw[i], NULL, key_worker, (void *)(long)(i + 1));
    for (int i = 0; i < 3; i++)
        pthread_join(kw[i], NULL);
    CHECK(destructor_runs == 6, "destructors ran for the workers: %d", destructor_runs);
    CHECK(pthread_getspecific(key) == (void *)100, "main retains its value");

    /* Once. */
    pthread_t ow[4];
    for (int i = 0; i < 4; i++)
        pthread_create(&ow[i], NULL, once_worker, NULL);
    for (int i = 0; i < 4; i++)
        pthread_join(ow[i], NULL);
    pthread_once(&once, once_init);
    CHECK(once_runs == 1, "once ran %d times", once_runs);

    /* Detached threads are reclaimed later. */
    int detached_done = 0;
    pthread_attr_t dattr;
    pthread_attr_init(&dattr);
    pthread_attr_setdetachstate(&dattr, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&dattr, 32768);
    for (int i = 0; i < 4; i++) {
        pthread_t d;
        CHECK(pthread_create(&d, &dattr, detached_worker, &detached_done) == 0, "create detached");
        CHECK(pthread_join(d, NULL) == EINVAL, "join of a detached thread is refused");
    }
    for (int i = 0; i < 100 && __atomic_load_n(&detached_done, __ATOMIC_SEQ_CST) < 4; i++)
        usleep(1000);
    CHECK(detached_done == 4, "detached threads ran: %d", detached_done);

    /* malloc and stdio from several threads. */
    pthread_t aligned;
    void *aligned_result = NULL;
    CHECK(pthread_create(&aligned, NULL, aligned_stack_worker, NULL) == 0, "create aligned worker");
    CHECK(pthread_join(aligned, &aligned_result) == 0 && aligned_result == (void *)1, "SIMD stack alignment");
    pthread_t fw[4];
    for (int i = 0; i < 4; i++)
        CHECK(pthread_create(&fw[i], NULL, file_worker, NULL) == 0, "create file worker");
    for (int i = 0; i < 4; i++) {
        void *result = NULL;
        CHECK(pthread_join(fw[i], &result) == 0 && result == (void *)1, "concurrent stream registry");
    }
    pthread_t mw[4];
    for (int i = 0; i < 4; i++)
        pthread_create(&mw[i], NULL, malloc_worker, (void *)(long)(i + 1));
    for (int i = 0; i < 4; i++)
        pthread_join(mw[i], NULL);
    pthread_t pw[3];
    for (int i = 0; i < 3; i++)
        pthread_create(&pw[i], NULL, print_worker, (void *)(long)i);
    for (int i = 0; i < 3; i++)
        pthread_join(pw[i], NULL);

    /* Spin locks and read-write locks. */
    pthread_spinlock_t spin;
    pthread_spin_init(&spin, PTHREAD_PROCESS_PRIVATE);
    CHECK(pthread_spin_lock(&spin) == 0 && pthread_spin_trylock(&spin) == EBUSY && pthread_spin_unlock(&spin) == 0, "spin lock");
    pthread_rwlock_t rw = PTHREAD_RWLOCK_INITIALIZER;
    counter = 1;
    pthread_t rr[3], ww;
    for (int i = 0; i < 3; i++)
        pthread_create(&rr[i], NULL, rw_reader, &rw);
    pthread_create(&ww, NULL, rw_writer, &rw);
    for (int i = 0; i < 3; i++)
        pthread_join(rr[i], NULL);
    pthread_join(ww, NULL);
    CHECK(rw.readers == 0 && rw.writer == 0, "rwlock released");

    printf("pthreadtest: %d failures\n", failures);
    return failures ? 1 : 0;
}
