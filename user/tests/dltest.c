/* Thread local storage, dlopen and lazy binding through /lib/ld.so. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdint.h>
#include <sys/wait.h>
#include <errno.h>

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("dltest: FAIL " __VA_ARGS__); printf("\n"); } } while (0)

/* The plugin appends its events here. */
char dltest_events[64];

/* Libraries loaded at start. */
extern __thread int tls_counter;
extern __thread char tls_buffer[64];
extern int *tls_counter_address(void);
extern char *tls_buffer_address(void);
extern int tls_ie_get(void);
extern void tls_ie_set(int value);
extern int tls_ld_next(void);

/* The program's own variables use the local-exec model. */
static __thread int local_value = 3;
static __thread char local_zero[32];

/* A worker thread that runs functions handed to it, so that a thread
 * created before dlopen can use the storage of an object loaded later. */
static void *(*volatile worker_job)(void *);
static void *volatile worker_arg, *volatile worker_result;
static volatile int worker_done, worker_quit;

static void *worker(void *arg)
{
    for (;;) {
        while (!worker_job && !worker_quit)
            usleep(1000);
        if (worker_quit)
            return NULL;
        worker_result = worker_job(worker_arg);
        worker_job = NULL;
        __atomic_store_n(&worker_done, 1, __ATOMIC_RELEASE);
    }
}

static void *run_in_worker(void *(*job)(void *), void *arg)
{
    worker_done = 0;
    worker_arg = arg;
    __atomic_store_n(&worker_job, job, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&worker_done, __ATOMIC_ACQUIRE))
        usleep(1000);
    return worker_result;
}

static void *read_static_tls(void *arg)
{
    static long values[4];
    values[0] = local_value;
    values[1] = tls_counter;
    values[2] = tls_ie_get();
    values[3] = (long)(intptr_t)tls_counter_address();
    local_value = 100;
    tls_counter = 200;
    tls_ie_set(300);
    return values;
}

static void test_static_tls(void)
{
    CHECK(local_value == 3 && local_zero[0] == 0, "program TLS initial values");
    CHECK(tls_counter == 5 && strcmp(tls_buffer, "image") == 0, "library TLS initial values");
    CHECK(tls_ie_get() == 7, "initial-exec variable");
    CHECK(tls_counter_address() == &tls_counter, "the program and the library agree on the address");
    CHECK(tls_buffer_address() == tls_buffer, "same for the array");
    CHECK(tls_ld_next() == 1 && tls_ld_next() == 2, "local dynamic variable");
    local_value = 10;
    tls_counter = 20;
    tls_ie_set(30);
    strcpy(tls_buffer, "main");
    long *values = run_in_worker(read_static_tls, NULL);
    CHECK(values[0] == 3 && values[1] == 5 && values[2] == 7, "the worker starts from the images: %ld %ld %ld",
          values[0], values[1], values[2]);
    CHECK((int *)(intptr_t)values[3] != &tls_counter, "the worker has its own block");
    CHECK(local_value == 10 && tls_counter == 20 && tls_ie_get() == 30 && strcmp(tls_buffer, "main") == 0,
          "the worker's writes stay in its block");
    CHECK(tls_ld_next() == 3, "local dynamic variable is per thread");
    values = run_in_worker(read_static_tls, NULL);
    CHECK(values[0] == 100 && values[1] == 200 && values[2] == 300, "the worker keeps its values");
}

static void *read_plugin_tls(void *arg)
{
    int *(*address)(void) = arg;
    int *p = address();
    int value = *p;
    *p = value + 1;
    return (void *)(intptr_t)value;
}

static void test_dlopen(void)
{
    CHECK(dlopen("libldabsent.so", RTLD_NOW) == NULL, "a missing library fails");
    char *error = dlerror();
    CHECK(error && strstr(error, "cannot open library"), "and names the failure: %s", error ? error : "(null)");
    CHECK(dlerror() == NULL, "dlerror clears the text");
    CHECK(dlopen("/bin/dltest", RTLD_NOW) == NULL, "an executable is not a shared object");
    error = dlerror();
    CHECK(error && strstr(error, "invalid ELF64 shared object"), "with its diagnostic: %s", error ? error : "(null)");

    void *libc = dlopen("libc.so", RTLD_NOW);
    CHECK(libc != NULL, "a library loaded at start is found");
    CHECK(dlsym(libc, "printf") == (void *)printf, "and its symbols resolve to the loaded copies");
    CHECK(dlsym(RTLD_DEFAULT, "tls_ie_get") == (void *)tls_ie_get, "the global scope holds the start libraries");
    CHECK(dlsym(libc, "no_such_symbol") == NULL && strstr(dlerror(), "undefined symbol"), "an unknown symbol fails");
    CHECK(dlclose(libc) == 0, "closing a start library is a no-op");

    dltest_events[0] = '\0';
    void *plugin = dlopen("libldplugin.so", RTLD_NOW);
    CHECK(plugin != NULL, "dlopen of the plugin: %s", plugin ? "ok" : dlerror());
    if (!plugin)
        return;
    CHECK(strcmp(dltest_events, "dp") == 0, "the dependency initialized before the plugin: '%s'", dltest_events);
    int (*count)(void) = (int (*)(void))dlsym(plugin, "plugin_count");
    long (*compute)(long) = (long (*)(long))dlsym(plugin, "plugin_compute");
    int *(*tls_address)(void) = (int *(*)(void))dlsym(plugin, "plugin_tls_address");
    CHECK(count && compute && tls_address, "dlsym finds the plugin's functions");
    if (!count || !compute || !tls_address)
        return;
    CHECK(count() == 1 && count() == 2, "the plugin's static data");
    CHECK(compute(4) == 8 + tls_counter, "the plugin calls its dependency and a start library");
    CHECK(dlsym(plugin, "plugdep_double") != NULL, "dlsym searches the plugin's dependencies");
    CHECK(dlsym(RTLD_DEFAULT, "plugin_count") == NULL, "RTLD_LOCAL keeps the plugin out of the global scope");
    dlerror();
    int *main_tls = tls_address();
    CHECK(*main_tls == 30, "dynamic TLS starts from the image in the main thread");
    *main_tls = 31;
    long *dep_value = dlsym(plugin, "plugdep_value");
    CHECK(dep_value && *dep_value == 21 + 4, "dlsym of a TLS symbol gives this thread's copy");
    int worker_value = (int)(intptr_t)run_in_worker(read_plugin_tls, (void *)tls_address);
    CHECK(worker_value == 30, "a thread created before dlopen gets its own block: %d", worker_value);
    CHECK(*main_tls == 31, "the main thread's copy is untouched");
    worker_value = (int)(intptr_t)run_in_worker(read_plugin_tls, (void *)tls_address);
    CHECK(worker_value == 31, "the worker's block persists: %d", worker_value);

    void *again = dlopen("libldplugin.so", RTLD_NOW | RTLD_GLOBAL);
    CHECK(again == plugin, "dlopen of a loaded library returns the same handle");
    CHECK(dlsym(RTLD_DEFAULT, "plugin_count") == (void *)count, "RTLD_GLOBAL promotes it to the global scope");
    CHECK(strcmp(dltest_events, "dp") == 0, "without running the constructors again");
    CHECK(dlclose(again) == 0 && strcmp(dltest_events, "dp") == 0, "the first dlclose only drops a reference");
    CHECK(count() == 3, "and the plugin still works");
    CHECK(dlclose(plugin) == 0, "the second dlclose unloads");
    CHECK(strcmp(dltest_events, "dpPD") == 0, "finalizers ran in reverse order: '%s'", dltest_events);
    CHECK(dlsym(RTLD_DEFAULT, "plugin_count") == NULL, "the unloaded plugin left the global scope");
    dlerror();

    /* Load it again: a fresh copy with fresh static data, and the TLS
     * slot may be reused, so a thread that used the old block must get
     * a new one initialized from the image. */
    dltest_events[0] = '\0';
    plugin = dlopen("libldplugin.so", RTLD_LAZY);
    CHECK(plugin != NULL, "dlopen after dlclose: %s", plugin ? "ok" : dlerror());
    if (!plugin)
        return;
    CHECK(strcmp(dltest_events, "dp") == 0, "constructors ran again: '%s'", dltest_events);
    count = (int (*)(void))dlsym(plugin, "plugin_count");
    tls_address = (int *(*)(void))dlsym(plugin, "plugin_tls_address");
    compute = (long (*)(long))dlsym(plugin, "plugin_compute");
    CHECK(count && count() == 1, "static data starts over");
    CHECK(tls_address && *tls_address() == 30, "main thread TLS starts over");
    worker_value = (int)(intptr_t)run_in_worker(read_plugin_tls, (void *)tls_address);
    CHECK(worker_value == 30, "the worker's stale block was replaced: %d", worker_value);
    CHECK(compute && compute(1) == 2 + tls_counter, "lazy binding inside a dlopen'd library");
    CHECK(dlclose(plugin) == 0 && strcmp(dltest_events, "dpPD") == 0, "unloaded again: '%s'", dltest_events);

    /* dlopen keeps its own copy of the name: a plugin host that reuses
     * one buffer for its paths must get the library the buffer names now.
     * The plugin is opened first, so that the second path names its
     * dependency, which is loaded already and is found by name. */
    char name[64];
    strcpy(name, "/lib/libldplugin.so");
    plugin = dlopen(name, RTLD_NOW);
    strcpy(name, "/lib/libldplugdep.so");
    void *dep = dlopen(name, RTLD_NOW);
    CHECK(plugin && dep && dep != plugin, "a reused name buffer names the second library");
    CHECK(dep && dlsym(dep, "plugdep_double") != NULL, "and that handle is the dependency");
    if (dep)
        dlclose(dep);
    if (plugin)
        dlclose(plugin);
}

static int run_child(const char *path, char *output, size_t capacity)
{
    int fds[2];
    if (pipe(fds) < 0)
        return -1;
    pid_t child = fork();
    if (child == 0) {
        close(fds[0]);
        dup2(fds[1], 1);
        dup2(fds[1], 2);
        close(fds[1]);
        execl(path, path, NULL);
        _exit(126);
    }
    close(fds[1]);
    size_t used = 0;
    for (;;) {
        ssize_t n = read(fds[0], output + used, capacity - used - 1);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        used += (size_t)n;
    }
    close(fds[0]);
    output[used] = '\0';
    int status;
    if (child < 0 || waitpid(child, &status, 0) != child || !WIFEXITED(status))
        return -1;
    return WEXITSTATUS(status);
}

static void test_lazy(void)
{
    char output[256];
    int status = run_child("/bin/ldlazy", output, sizeof output);
    CHECK(status == 0 && strcmp(output, "lazy 1 1 4.00 5\n") == 0, "lazy binding: exit %d, output '%s'", status, output);
}

int main(void)
{
    pthread_t thread;
    CHECK(pthread_create(&thread, NULL, worker, NULL) == 0, "worker thread");
    test_static_tls();
    test_dlopen();
    test_lazy();
    worker_quit = 1;
    pthread_join(thread, NULL);
    printf("dltest: %d failures\n", failures);
    return failures ? 1 : 0;
}
