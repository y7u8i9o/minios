/* xfer between the guest and the host in both directions. tools/xfer.py
 * serves on NETPEER_PORT, reached as 10.0.2.2; the guest then serves on
 * 9100, which the harness forwards, and waits for the host to store a
 * file there. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>

static int failures;
#define CHECK(c, text) do { if (!(c)) { printf("xferhosttest: FAIL %s\n", text); failures++; } } while (0)

static int run(const char *const *args)
{
    pid_t pid = fork();
    if (pid == 0) { execv(args[0], (char *const *)args); _exit(127); }
    int status = -1;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
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
    const char *port = getenv("NETPEER_PORT");
    CHECK(port != NULL, "host port supplied");
    if (!port) return 1;
    const char *const dhcp[] = {"/bin/dhcpc", "-1", "-t", "30", NULL};
    CHECK(run(dhcp) == 0, "lease from the QEMU DHCP server");
    mkdir("/tmp", 0755);
    mkdir("/tmp/srv", 0755);
    FILE *f = fopen("/tmp/up.bin", "w");
    for (int i = 0; i < 300000; i++) fputc((i * 31) & 255, f);
    fclose(f);
    const char *const get[] = {"/bin/xfer", "get", "-p", port, "-o", "/tmp", "greeting.txt", NULL};
    CHECK(run(get) == 0, "get from the host");
    char line[64] = "";
    f = fopen("/tmp/greeting.txt", "r");
    if (f) { fgets(line, sizeof line, f); fclose(f); }
    CHECK(strcmp(line, "hello from the host\n") == 0, "host file content");
    const char *const put[] = {"/bin/xfer", "put", "-p", port, "/tmp/up.bin", NULL};
    CHECK(run(put) == 0, "put to the host");
    const char *const back[] = {"/bin/xfer", "get", "-p", port, "-o", "/tmp", "up.bin", NULL};
    rename("/tmp/up.bin", "/tmp/orig.bin");
    CHECK(run(back) == 0, "get it back");
    CHECK(same("/tmp/orig.bin", "/tmp/up.bin"), "round trip bytes match");

    /* Now the guest serves and the host pushes and pulls through the
     * forwarded port; the host's peer script finishes by storing
     * fromhost.txt here. */
    pid_t server = fork();
    if (server == 0) {
        const char *const args[] = {"/bin/xfer", "serve", "/tmp/srv", NULL};
        execv(args[0], (char *const *)args);
        _exit(127);
    }
    int seen = 0;
    for (int i = 0; i < 600 && !seen; i++) {
        usleep(100000);
        f = fopen("/tmp/srv/fromhost.txt", "r");
        if (f) { seen = 1; fclose(f); }
    }
    CHECK(seen, "the host stored a file on the guest");
    kill(server, SIGTERM);
    waitpid(server, NULL, 0);
    printf("xferhosttest: %d failures\n", failures);
    return failures ? 1 : 0;
}
