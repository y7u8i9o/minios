/* Checks of dynamic linking: the program shares the C library's data and
 * functions through the loader, and the library lives above the program
 * in the address space. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <signal.h>

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("dyntest: FAIL " __VA_ARGS__); printf("\n"); } } while (0)

static int compare(const void *a, const void *b)
{
    return *(const int *)a - *(const int *)b;
}

/* Capture both streams and require a normal exit. In particular, a loader
 * segmentation fault must never pass a test expecting status 127. Drain
 * excess output so a broken child cannot block on a full pipe. */
static int run_child(const char *path, const char *arg, char *output, size_t capacity)
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
        if (arg)
            execl(path, path, arg, NULL);
        else
            execl(path, path, NULL);
        _exit(126);
    }
    close(fds[1]);
    size_t used = 0;
    char buf[256];
    for (;;) {
        ssize_t n = read(fds[0], buf, sizeof buf);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        size_t copy = (size_t)n;
        if (copy > capacity - used - 1)
            copy = capacity - used - 1;
        memcpy(output + used, buf, copy);
        used += copy;
    }
    close(fds[0]);
    output[used] = '\0';
    int status;
    if (child < 0 || waitpid(child, &status, 0) != child)
        return -1;
    if (WIFSIGNALED(status))
        return -WTERMSIG(status);
    if (!WIFEXITED(status))
        return -1;
    return WEXITSTATUS(status);
}

static void test_lifecycle(void)
{
    char output[512];
    int status = run_child("/bin/ldlifecycle", NULL, output, sizeof output);
    CHECK(status == 0 && strcmp(output, "p!abcdefghijklmnopqrstuvmxVUTSRQPONMLKJIHGFEDCBA?") == 0,
          "dynamic lifecycle: exit %d, output '%s'", status, output);
    status = run_child("/bin/ldlifecycle", "quick", output, sizeof output);
    CHECK(status == 0 && strcmp(output, "p!abcdefghijklmnopqrstuvm") == 0,
          "_exit bypasses destructors: exit %d, output '%s'", status, output);
    status = run_child("/bin/ldstatic", NULL, output, sizeof output);
    CHECK(status == 0 && strcmp(output, "puvmxVU") == 0,
          "static lifecycle: exit %d, output '%s'", status, output);
    status = run_child("/bin/ldbad0000", "relro", output, sizeof output);
    CHECK(status == -SIGSEGV, "RELRO rejects writes: status %d, output '%s'", status, output);
}

static void test_elf_fixtures(void)
{
    FILE *manifest = fopen("/usr/share/ldtests/cases", "r");
    CHECK(manifest != NULL, "open ELF fixture manifest");
    if (!manifest)
        return;
    char line[256], output[512], path[64];
    unsigned count = 0;
    while (fgets(line, sizeof line, manifest)) {
        char *code = strchr(line, '|');
        char *diagnostic = code ? strchr(code + 1, '|') : NULL;
        char *name = diagnostic ? strchr(diagnostic + 1, '|') : NULL;
        CHECK(name != NULL, "valid fixture manifest entry");
        if (!name)
            break;
        *code++ = '\0';
        *diagnostic++ = '\0';
        *name++ = '\0';
        name[strcspn(name, "\n")] = '\0';
        snprintf(path, sizeof path, "/bin/ldbad%04d", atoi(line));
        int expected = atoi(code);
        int status = run_child(path, NULL, output, sizeof output);
        CHECK(status == expected, "%s: expected exit %d, got %d, output '%s'",
              name, expected, status, output);
        CHECK(strcmp(diagnostic, "-") == 0 || strstr(output, diagnostic) != NULL,
              "%s: missing diagnostic '%s', output '%s'", name, diagnostic, output);
        count++;
    }
    fclose(manifest);
    CHECK(count >= 30, "fixture manifest was complete (%u cases)", count);
    printf("dyntest: %u ELF fixtures checked\n", count);
}

int main(int argc, char **argv)
{
    /* Library code is mapped by the loader far above the program text. */
    CHECK((uintptr_t)printf > 0x100000000UL, "printf is in a shared library at %p", (void *)printf);
    CHECK((uintptr_t)main < 0x1000000UL, "main is in the program at %p", (void *)main);

    /* Data shared between the program and the library. */
    setenv("DYNTEST", "shared", 1);
    int found = 0;
    for (char **e = environ; *e != NULL; e++)
        if (strcmp(*e, "DYNTEST=shared") == 0)
            found = 1;
    CHECK(found, "environ contains what setenv wrote");
    CHECK(fileno(stdout) == 1 && fileno(stderr) == 2, "stdout and stderr are the library's streams");
    optind = 1;
    char *args[] = { "dyntest", "-x", NULL };
    CHECK(getopt(2, args, "x") == 'x' && optind == 2, "getopt reads and writes optind");
    errno = 0;
    CHECK(close(999) < 0 && errno == EBADF, "errno reaches the program");

    /* A callback from the library into the program. */
    int values[] = { 3, 1, 2 };
    qsort(values, 3, sizeof values[0], compare);
    CHECK(values[0] == 1 && values[2] == 3, "qsort calls the program's comparator");

    /* A dynamically linked child. */
    pid_t pid = fork();
    if (pid == 0) {
        execlp("test", "test", "1", NULL);
        _exit(127);
    }
    int status = -1;
    CHECK(pid > 0 && waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "exec of a dynamically linked program");

    test_lifecycle();
    test_elf_fixtures();
    printf("dyntest: %d failures\n", failures);
    return failures ? 1 : 0;
}
