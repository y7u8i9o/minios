/* xfer over loopback: a server in a child; put and get of a file and of a
 * directory tree, ls, checksum and path checks. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>

static int failures;
#define CHECK(c, text) do { if (!(c)) { printf("xfertest: FAIL %s\n", text); failures++; } } while (0)

static int run(const char *const *args, char *out, size_t size)
{
    int p[2];
    pipe(p);
    pid_t pid = fork();
    if (pid == 0) {
        dup2(p[1], 1);
        close(p[0]); close(p[1]);
        execv(args[0], (char *const *)args);
        _exit(127);
    }
    close(p[1]);
    size_t got = 0;
    ssize_t n;
    while (out && got + 1 < size && (n = read(p[0], out + got, size - 1 - got)) > 0)
        got += (size_t)n;
    if (out) out[got] = 0;
    close(p[0]);
    int status = -1;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static void fill(const char *path, int n, int seed)
{
    FILE *f = fopen(path, "w");
    for (int i = 0; i < n; i++) fputc((i * seed + (i >> 8)) & 255, f);
    fclose(f);
}

static int same(const char *a, const char *b)
{
    FILE *fa = fopen(a, "r"), *fb = fopen(b, "r");
    int equal = fa && fb;
    while (equal) { int x = fgetc(fa), y = fgetc(fb); equal = x == y; if (x == EOF) break; }
    if (fa) fclose(fa);
    if (fb) fclose(fb);
    return equal;
}

int main(void)
{
    mkdir("/tmp", 0755);
    mkdir("/tmp/xin", 0755);
    mkdir("/tmp/xin/tree", 0755);
    mkdir("/tmp/xin/tree/sub", 0755);
    mkdir("/tmp/xsrv", 0755);
    mkdir("/tmp/xout", 0755);
    fill("/tmp/xin/data.bin", 200000, 37);
    fill("/tmp/xin/tree/a.bin", 5000, 3);
    fill("/tmp/xin/tree/sub/b.bin", 70000, 11);
    fill("/tmp/xin/tree/sub/empty", 0, 1);
    fill("/tmp/xin/tree/with space.wav", 3000, 7);
    pid_t server = fork();
    if (server == 0) {
        const char *const args[] = {"/bin/xfer", "serve", "-p", "9100", "/tmp/xsrv", NULL};
        execv(args[0], (char *const *)args);
        _exit(127);
    }
    usleep(200000);
    char out[2048];
    const char *const put[] = {"/bin/xfer", "put", "-h", "127.0.0.1", "/tmp/xin/data.bin", NULL};
    CHECK(run(put, out, sizeof out) == 0 && strstr(out, "sent data.bin, 200000 bytes"), "put a file");
    CHECK(same("/tmp/xin/data.bin", "/tmp/xsrv/data.bin"), "stored bytes match");
    const char *const puttree[] = {"/bin/xfer", "put", "-h", "localhost", "/tmp/xin/tree/", NULL};
    CHECK(run(puttree, out, sizeof out) == 0, "put a directory");
    CHECK(same("/tmp/xin/tree/sub/b.bin", "/tmp/xsrv/tree/sub/b.bin"), "nested file stored");
    CHECK(same("/tmp/xin/tree/with space.wav", "/tmp/xsrv/tree/with space.wav"), "name with a space stored");
    const char *const ls[] = {"/bin/xfer", "ls", "-h", "127.0.0.1", "tree/sub", NULL};
    CHECK(run(ls, out, sizeof out) == 0 && strstr(out, "70000  b.bin") && strstr(out, "0  empty"), "ls");
    const char *const get[] = {"/bin/xfer", "get", "-h", "127.0.0.1", "-o", "/tmp/xout", "data.bin", "tree", NULL};
    CHECK(run(get, out, sizeof out) == 0, "get a file and a directory");
    CHECK(same("/tmp/xin/data.bin", "/tmp/xout/data.bin") && same("/tmp/xin/tree/sub/b.bin", "/tmp/xout/tree/sub/b.bin") &&
          same("/tmp/xin/tree/a.bin", "/tmp/xout/tree/a.bin") &&
          same("/tmp/xin/tree/with space.wav", "/tmp/xout/tree/with space.wav"), "fetched bytes match");
    const char *const missing[] = {"/bin/xfer", "get", "-h", "127.0.0.1", "-o", "/tmp/xout", "nothere", NULL};
    CHECK(run(missing, out, sizeof out) == 1, "missing file reported");
    const char *const escape[] = {"/bin/xfer", "get", "-h", "127.0.0.1", "-o", "/tmp/xout", "../xin/data.bin", NULL};
    CHECK(run(escape, out, sizeof out) == 1, "path with .. refused");
    const char *const refused[] = {"/bin/xfer", "ls", "-h", "127.0.0.1", "-p", "9101", NULL};
    CHECK(run(refused, out, sizeof out) == 1, "no server reported");
    kill(server, SIGTERM);
    waitpid(server, NULL, 0);
    printf("xfertest: %d failures\n", failures);
    return failures ? 1 : 0;
}
