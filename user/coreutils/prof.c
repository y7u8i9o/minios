/* prof: sampling profiler front end.
 *   prof [-d seconds] [-k] [-c] [-n top] command [args...]
 *   prof -p pid [-d seconds] [-k] [-c] [-n top]
 * Samples come from /dev/profile at the timer rate on every CPU. User
 * addresses are symbolized with the .symtab of /bin/<name>, kernel
 * addresses (-k) with /dev/ksyms. -c prints the most frequent call chains. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/ipc.h>
#include <minios/profile.h>

static struct prof_hist flat, chains;
static struct prof_symtab *usyms, *ksyms;
static int want_kernel, want_chains;
static int gate[2];             /* the child waits here until sampling runs */

static void consume(int fd)
{
    struct prof_sample buf[256];
    ssize_t n;
    while ((n = prof_read(fd, buf, 256)) > 0) {
        for (ssize_t i = 0; i < n; i++) {
            struct prof_sample *s = &buf[i];
            int kernel = !(s->flags & PROF_FLAG_USER);
            if (kernel && !want_kernel)
                continue;
            char name[128];
            prof_format_addr(usyms, ksyms, kernel, s->chain[0], 0, name, sizeof name);
            prof_hist_add(&flat, name, kernel);
            if (want_chains) {
                char chain[1024];
                size_t used = 0;
                for (unsigned d = 0; d < s->depth && used < sizeof chain - 130; d++) {
                    prof_format_addr(usyms, ksyms, kernel, s->chain[d], 0, name, sizeof name);
                    used += (size_t)snprintf(chain + used, sizeof chain - used, "%s%s", d ? " < " : "", name);
                }
                prof_hist_add(&chains, chain, kernel);
            }
        }
    }
}

static void report(struct prof_hist *h, const char *title, int top)
{
    prof_hist_sort(h);
    printf("\n%s (%u samples)\n", title, h->total);
    printf("%7s %8s  %s\n", "percent", "samples", "symbol");
    for (size_t i = 0; i < h->count && (int)i < top; i++) {
        struct prof_bucket *b = &h->buckets[i];
        printf("%6.1f%% %8u  %s%s\n", 100.0 * b->count / h->total, b->count, b->key,
               b->kernel == b->count ? " [kernel]" : b->kernel ? " [mixed]" : "");
    }
}

static char *proc_name(pid_t pid)
{
    FILE *f = fopen("/dev/proc", "r");
    if (!f)
        return NULL;
    static char line[256];
    char *result = NULL;
    while (fgets(line, sizeof line, f)) {
        char *end;
        long p = strtol(line, &end, 10);
        if (end == line || p != pid)
            continue;
        /* PID PPID PGID STATE TIME RSS NAME */
        char *tok = end;
        for (int col = 0; col < 5; col++) {
            while (*tok == ' ') tok++;
            while (*tok && *tok != ' ') tok++;
        }
        while (*tok == ' ') tok++;
        tok[strcspn(tok, "\n")] = '\0';
        result = tok;
        break;
    }
    fclose(f);
    return result;
}

static void usage(void)
{
    fprintf(stderr, "usage: prof [-d seconds] [-k] [-c] [-n top] command [args...]\n"
                    "       prof -p pid [-d seconds] [-k] [-c] [-n top]\n");
    exit(2);
}

int main(int argc, char **argv)
{
    double duration = 0;
    int top = 25;
    pid_t pid = 0;
    int i = 1;
    for (; i < argc && argv[i][0] == '-'; i++) {
        if (strcmp(argv[i], "-k") == 0) want_kernel = 1;
        else if (strcmp(argv[i], "-c") == 0) want_chains = 1;
        else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) duration = atof(argv[++i]);
        else if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) top = atoi(argv[++i]);
        else if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) pid = atoi(argv[++i]);
        else usage();
    }
    if (!pid && i >= argc)
        usage();
    if (pid && duration <= 0)
        duration = 5;

    int fd = prof_open();
    if (fd < 0) {
        fprintf(stderr, "prof: /dev/profile: %s\n", strerror(errno));
        return 1;
    }
    const char *binary_name;
    pid_t child = 0;
    if (pid) {
        binary_name = proc_name(pid);
        if (!binary_name) {
            fprintf(stderr, "prof: no process %d\n", pid);
            return 1;
        }
    } else {
        const char *slash = strrchr(argv[i], '/');
        binary_name = slash ? slash + 1 : argv[i];
        if (pipe(gate) < 0) {
            fprintf(stderr, "prof: pipe: %s\n", strerror(errno));
            return 1;
        }
        child = fork();
        if (child < 0) {
            fprintf(stderr, "prof: fork: %s\n", strerror(errno));
            return 1;
        }
        if (child == 0) {
            /* Wait for the parent to start sampling before running. */
            char go;
            close(gate[1]);
            read(gate[0], &go, 1);
            close(gate[0]);
            execvp(argv[i], argv + i);
            fprintf(stderr, "prof: %s: %s\n", argv[i], strerror(errno));
            _exit(127);
        }
        close(gate[0]);
        pid = child;
    }
    char path[128];
    snprintf(path, sizeof path, "/bin/%s", binary_name);
    usyms = prof_symtab_load_elf(path);
    if (!usyms)
        fprintf(stderr, "prof: no symbols in %s, user addresses stay numeric\n", path);
    if (want_kernel)
        ksyms = prof_symtab_load_kernel();
    prof_hist_init(&flat);
    prof_hist_init(&chains);

    if (prof_start(fd, pid) < 0) {
        fprintf(stderr, "prof: start: %s\n", strerror(errno));
        return 1;
    }
    int status = 0;
    if (child) {
        write(gate[1], "g", 1);
        close(gate[1]);
        for (;;) {
            struct pollfd pfd = { fd, POLLIN, 0 };
            poll(&pfd, 1, 200);
            consume(fd);
            pid_t r = waitpid(child, &status, WNOHANG);
            if (r == child)
                break;
        }
    } else {
        long ms = (long)(duration * 1000);
        while (ms > 0) {
            struct pollfd pfd = { fd, POLLIN, 0 };
            poll(&pfd, 1, ms < 200 ? (int)ms : 200);
            consume(fd);
            ms -= 200;
        }
    }
    prof_stop(fd);
    consume(fd);
    struct prof_stats st;
    prof_get_stats(fd, &st);
    printf("prof: %lu samples, %lu dropped, pid %d (%s)\n", (unsigned long)st.samples, (unsigned long)st.dropped,
           pid, binary_name);
    report(&flat, "Flat profile", top);
    if (want_chains)
        report(&chains, "Call chains", top);
    close(fd);
    if (child)
        return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
    return 0;
}
