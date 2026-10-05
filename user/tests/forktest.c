/* M9 test: ELF loading with arguments, copy on write fork, exec, wait4,
 * kill, getppid and user threads. Exits 0 on success. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/thread.h>

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

static char big[256 * 1024];
static int counter;
static char thread_stacks[2][16384];

static void thread_fn(void *arg)
{
    int id = (int)(long)arg;
    for (int i = 0; i < 1000; i++)
        __atomic_add_fetch(&counter, 1, __ATOMIC_SEQ_CST);
    thread_exit(id + 100);
}

int main(int argc, char **argv, char **envp)
{
    printf("forktest: pid %d ppid %d\n", getpid(), getppid());
    CHECK(argc == 3, "argc %d", argc);
    CHECK(strcmp(argv[1], "arg1") == 0 && strcmp(argv[2], "arg two") == 0, "argv");
    CHECK(getenv("TEST") && strcmp(getenv("TEST"), "1") == 0, "envp");
    CHECK(envp[0] != NULL, "envp vector");

    /* Copy on write: the child changes memory the parent must not see. */
    memset(big, 0x11, sizeof big);
    int *heap = malloc(1000 * sizeof(int));
    for (int i = 0; i < 1000; i++)
        heap[i] = i;
    int local = 5;
    pid_t pid = fork();
    CHECK(pid >= 0, "fork failed: %s", strerror(errno));
    if (pid == 0) {
        CHECK(getppid() != 0, "child ppid");
        memset(big, 0x22, sizeof big);
        for (int i = 0; i < 1000; i++)
            heap[i] = -i;
        local = 6;
        char *p = malloc(100000);
        memset(p, 7, 100000);
        _exit(failures ? 9 : 3 + local);
    }
    int status = -1;
    pid_t got = wait4(pid, &status, 0, NULL);
    CHECK(got == pid, "wait4 returned %d, expected %d", got, pid);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 9, "child status 0x%x", status);
    int ok = 1;
    for (size_t i = 0; i < sizeof big; i += 4096)
        if (big[i] != 0x11) ok = 0;
    for (int i = 0; i < 1000; i++)
        if (heap[i] != i) ok = 0;
    CHECK(ok && local == 5, "parent memory changed by child");

    /* Parent writes after fork must not affect a still running child. */
    pid = fork();
    if (pid == 0) {
        sched_yield();
        sched_yield();
        _exit(big[4096] == 0x11 ? 4 : 5);
    }
    big[4096] = 0x33;
    wait4(pid, &status, 0, NULL);
    CHECK(WEXITSTATUS(status) == 4, "child saw parent's later write, status 0x%x", status);

    /* exec of another program from the initrd, with arguments. */
    pid = fork();
    if (pid == 0) {
        char *const args[] = { "/bin/hello", "one", "two", NULL };
        execve("/bin/hello", args, envp);
        _exit(100);
    }
    wait4(pid, &status, 0, NULL);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 7, "exec hello status 0x%x", status);

    /* exec failures. */
    char *const noargs[] = { "x", NULL };
    CHECK(execve("/bin/does-not-exist", noargs, envp) < 0 && errno == ENOENT, "execve missing file");

    /* kill terminates a spinning child. */
    pid = fork();
    if (pid == 0) {
        for (;;)
            ;
    }
    sched_yield();
    CHECK(kill(pid, SIGTERM) == 0, "kill: %s", strerror(errno));
    wait4(pid, &status, 0, NULL);
    CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGTERM, "killed status 0x%x", status);

    /* kill a child blocked reading the keyboard. */
    pid = fork();
    if (pid == 0) {
        char c;
        read(0, &c, 1);
        _exit(66);
    }
    sched_yield();
    kill(pid, SIGKILL);
    wait4(pid, &status, 0, NULL);
    CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL, "blocked child status 0x%x", status);

    /* A crashing child reports SIGSEGV. */
    pid = fork();
    if (pid == 0) {
        /* The address passes through a volatile variable. The compiler then
         * cannot prove the store invalid at compile time. */
        volatile uintptr_t bad = 8;
        *(volatile int *)bad = 1;
        _exit(0);
    }
    wait4(pid, &status, 0, NULL);
    CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV, "segv status 0x%x", status);

    /* wait with no children. */
    CHECK(wait4(-1, &status, 0, NULL) < 0 && errno == ECHILD, "wait4 without children");

    /* Threads sharing the address space. */
    thread_t t[2];
    for (int i = 0; i < 2; i++)
        CHECK(thread_create(&t[i], thread_fn, (void *)(long)i, thread_stacks[i], sizeof thread_stacks[i]) == 0,
              "thread_create %d", i);
    for (int i = 0; i < 2; i++) {
        int code = -1;
        CHECK(thread_join(t[i], &code) == 0 && code == i + 100, "thread_join %d code %d", i, code);
    }
    CHECK(counter == 2000, "counter %d", counter);

    /* Deep stack use within the 1 MiB region, demand paged. */
    char probe[300 * 1024];
    memset(probe, 1, sizeof probe);
    CHECK(probe[0] == 1 && probe[sizeof probe - 1] == 1, "stack probe");

    /* Heap growth through sbrk. */
    char *h = malloc(3 * 1024 * 1024);
    CHECK(h != NULL, "malloc 3 MiB");
    if (h) {
        memset(h, 9, 3 * 1024 * 1024);
        free(h);
    }
    char cwd[64];
    CHECK(getcwd(cwd, sizeof cwd) && strcmp(cwd, "/") == 0, "cwd %s", cwd);
    CHECK(chdir("/etc") == 0 && getcwd(cwd, sizeof cwd) && strcmp(cwd, "/etc") == 0, "chdir");
    CHECK(chdir("/nope") < 0 && errno == ENOENT, "chdir missing");
    CHECK(chdir("..") == 0 && getcwd(cwd, sizeof cwd) && strcmp(cwd, "/") == 0, "chdir ..");

    printf("forktest: %d failures\n", failures);
    return failures ? 1 : 0;
}
