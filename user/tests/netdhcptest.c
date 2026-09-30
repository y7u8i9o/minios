/* N10: DHCP client against a scripted server on loopback, then a live lease
 * from QEMU's user-mode DHCP server. Runs with nic user. N16 adds address
 * conflict detection, whose probes go out on the NIC even when the scripted
 * server answers on loopback (QEMU answers ARP for 10.0.2.2, so offering
 * that address produces a conflict), and INIT-REBOOT from a saved lease.
 * The scripted runs use -A, which shortens the RFC 5227 intervals, and
 * their own lease files; the live runs use the default timing and file. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/ipc.h>
#include <sys/socket.h>
#include <arpa/inet.h>

static int failures;
#define CHECK(c, text) do { if (!(c)) { printf("netdhcptest: FAIL %s (errno %d)\n", text, errno); failures++; } } while (0)
#define PORT 6767

static uint32_t get32(const unsigned char *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static void put32(unsigned char *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }

/* option32 returns a four-byte option of a request, or 0 when it is
 * absent. */
static uint32_t option32(const unsigned char *m, size_t len, int code)
{
    size_t o = 240;
    while (o + 2 <= len && m[o] != 255) {
        if (m[o] == 0) { o++; continue; }
        if (m[o] == code && m[o + 1] == 4 && o + 6 <= len)
            return get32(m + o + 2);
        o += 2 + m[o + 1];
    }
    return 0;
}

static int has_option(const unsigned char *m, size_t len, int code)
{
    size_t o = 240;
    while (o + 2 <= len && m[o] != 255) {
        if (m[o] == 0) { o++; continue; }
        if (m[o] == code)
            return 1;
        o += 2 + m[o + 1];
    }
    return 0;
}

static int file_has(const char *path, const char *text)
{
    FILE *f = fopen(path, "r");
    char line[128];
    int found = 0;
    while (f && fgets(line, sizeof line, f))
        found |= strstr(line, text) != NULL;
    if (f)
        fclose(f);
    return found;
}

/* Find option code in a request. */
static int option(const unsigned char *m, size_t len, int code)
{
    size_t o = 240;
    while (o + 2 <= len && m[o] != 255) {
        if (m[o] == 0) { o++; continue; }
        if (m[o] == code && m[o + 1] == 1)
            return m[o + 2];
        o += 2 + m[o + 1];
    }
    return -1;
}

static int server_fd;
static int server(void)
{
    server_fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in name = {.sin_family = AF_INET, .sin_port = htons(PORT),
                               .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    return bind(server_fd, (struct sockaddr *)&name, sizeof name);
}

/* drain discards requests still queued from an earlier client. */
static void drain(void)
{
    unsigned char m[1500];
    struct pollfd pfd = {.fd = server_fd, .events = POLLIN};
    while (poll(&pfd, 1, 0) == 1 && recv(server_fd, m, sizeof m, 0) >= 0)
        ;
}

/* await waits for one request of the given type, or of any type when type
 * is negative, and returns its length or 0. */
static size_t await(unsigned char *m, int type, int timeout_ms, struct sockaddr_in *from)
{
    for (;;) {
        struct pollfd pfd = {.fd = server_fd, .events = POLLIN};
        if (poll(&pfd, 1, timeout_ms) != 1)
            return 0;
        socklen_t len = sizeof *from;
        ssize_t n = recvfrom(server_fd, m, 1500, 0, (struct sockaddr *)from, &len);
        if (n >= 240 && (type < 0 || option(m, (size_t)n, 53) == type))
            return (size_t)n;
    }
}

static void reply(const unsigned char *request, size_t len, const struct sockaddr_in *to,
                  int type, uint32_t yiaddr, uint32_t xid_override, int with_mask,
                  unsigned lease, unsigned t1, unsigned t2)
{
    unsigned char m[300];
    memset(m, 0, sizeof m);
    memcpy(m, request, len < 240 ? len : 240);
    m[0] = 2;
    if (xid_override)
        put32(m + 4, xid_override);
    put32(m + 16, yiaddr);
    unsigned char *o = m + 236;
    memcpy(o, "\x63\x82\x53\x63", 4);
    o += 4;
    *o++ = 53; *o++ = 1; *o++ = (unsigned char)type;
    *o++ = 54; *o++ = 4; put32(o, 0x0a000202); o += 4;
    if (with_mask) { *o++ = 1; *o++ = 4; put32(o, 0xffffff00); o += 4; }
    *o++ = 3; *o++ = 4; put32(o, 0x0a000202); o += 4;
    *o++ = 6; *o++ = 4; put32(o, 0x0a000203); o += 4;
    *o++ = 15; *o++ = 12; memcpy(o, "example.test", 12); o += 12;
    *o++ = 51; *o++ = 4; put32(o, lease); o += 4;
    if (t1) { *o++ = 58; *o++ = 4; put32(o, t1); o += 4; }
    if (t2) { *o++ = 59; *o++ = 4; put32(o, t2); o += 4; }
    *o++ = 255;
    sendto(server_fd, m, sizeof m, 0, (const struct sockaddr *)to, sizeof *to);
}

static int inet_line(char *out, size_t size)
{
    FILE *f = fopen("/dev/net", "r");
    char line[512];
    int found = 0;
    out[0] = 0;
    while (f && fgets(line, sizeof line, f)) {
        if (strncmp(line, "inet ", 5) == 0) {
            strncpy(out, line, size - 1);
            found = 1;
        }
    }
    if (f)
        fclose(f);
    return found;
}

static int resolv_has(const char *text)
{
    FILE *f = fopen("/etc/resolv.conf", "r");
    char line[128];
    int found = 0;
    while (f && fgets(line, sizeof line, f))
        found |= strstr(line, text) != NULL;
    if (f)
        fclose(f);
    return found;
}

static pid_t spawn(const char *const *args, int *out_fd)
{
    int pipefd[2];
    pipe(pipefd);
    pid_t pid = fork();
    if (pid == 0) {
        dup2(pipefd[1], 1);
        close(pipefd[0]);
        close(pipefd[1]);
        execv(args[0], (char *const *)args);
        _exit(127);
    }
    close(pipefd[1]);
    *out_fd = pipefd[0];
    return pid;
}

static int wait_status(pid_t pid)
{
    int status = -1;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

int main(void)
{
    CHECK(server() == 0, "scripted server socket");
    unsigned char m[1500];
    struct sockaddr_in from;
    char line[512];

    /* 1. Server absent: bounded failure, nothing configured. */
    const char *const absent[] = {"/bin/dhcpc", "-1", "-t", "3", "-A", "-l", "/tmp/dhcp-1.lease",
                                  "-s", "127.0.0.1", "-p", "6768", NULL};
    int out;
    pid_t pid = spawn(absent, &out);
    CHECK(wait_status(pid) == 1, "no server gives up after -t seconds");
    close(out);
    CHECK(!inet_line(line, sizeof line) || strstr(line, "0.0.0.0/"), "absent server leaves no address");

    /* 2. Malformed and unrelated replies are ignored, then a valid lease. */
    const char *const once[] = {"/bin/dhcpc", "-1", "-t", "20", "-A", "-l", "/tmp/dhcp-2.lease",
                                "-s", "127.0.0.1", "-p", "6767", NULL};
    pid = spawn(once, &out);
    size_t n = await(m, 1, 5000, &from);
    CHECK(n, "discover received");
    if (n) {
        reply(m, n, &from, 2, 0x0a00020f, 0x12345678, 1, 600, 0, 0); /* wrong xid */
        reply(m, n, &from, 2, 0x0a00020f, 0, 0, 600, 0, 0);          /* no subnet mask */
        sendto(server_fd, "garbage", 7, 0, (struct sockaddr *)&from, sizeof from);
        reply(m, n, &from, 2, 0x0a00020f, 0, 1, 600, 0, 0);          /* valid offer */
    }
    n = await(m, 3, 5000, &from);
    CHECK(n, "request received");
    CHECK(n && get32(m + 4) == get32(m + 4), "request carries the transaction");
    if (n)
        reply(m, n, &from, 5, 0x0a00020f, 0, 1, 600, 0, 0);
    CHECK(wait_status(pid) == 0, "one-shot client bound");
    char text[256] = "";
    read(out, text, sizeof text - 1);
    close(out);
    CHECK(strstr(text, "bound 10.0.2.15 from 10.0.2.2 lease 600") != NULL, "lease reported");
    CHECK(inet_line(line, sizeof line) && strstr(line, "10.0.2.15/255.255.255.0 gw 10.0.2.2"),
          "lease applied through /dev/net");
    CHECK(resolv_has("nameserver 10.0.2.3"), "resolver configuration written");
    CHECK(resolv_has("search example.test"), "domain option written as the search list");

    /* 3. Renewal at T1, NAK on the next renewal removes the address. */
    const char *const daemon[] = {"/bin/dhcpc", "-f", "-A", "-l", "/tmp/dhcp-3.lease",
                                  "-s", "127.0.0.1", "-p", "6767", NULL};
    pid = spawn(daemon, &out);
    n = await(m, 1, 5000, &from);
    if (n)
        reply(m, n, &from, 2, 0x0a00020f, 0, 1, 20, 2, 4);
    n = await(m, 3, 5000, &from);
    if (n)
        reply(m, n, &from, 5, 0x0a00020f, 0, 1, 20, 2, 4);
    n = await(m, 3, 6000, &from); /* renewal, unicast with ciaddr */
    CHECK(n && get32(m + 12) == 0x0a00020f, "renewal at T1 carries ciaddr");
    if (n)
        reply(m, n, &from, 5, 0x0a00020f, 0, 1, 20, 2, 4);
    n = await(m, 3, 6000, &from);
    CHECK(n, "second renewal");
    if (n)
        reply(m, n, &from, 6, 0, 0, 1, 0, 0, 0);
    sleep(1);
    CHECK(inet_line(line, sizeof line) && strstr(line, " 0.0.0.0/"), "NAK removes the address");
    CHECK(!resolv_has("nameserver"), "NAK clears the resolver configuration");

    /* 4. Expiry without a reachable server. */
    n = await(m, 1, 10000, &from);
    CHECK(n, "rediscovery after NAK");
    if (n)
        reply(m, n, &from, 2, 0x0a00020f, 0, 1, 10, 3, 5);
    n = await(m, 3, 5000, &from);
    if (n)
        reply(m, n, &from, 5, 0x0a00020f, 0, 1, 10, 3, 5);
    sleep(2); /* conflict detection runs before the address is applied */
    CHECK(inet_line(line, sizeof line) && strstr(line, "10.0.2.15/"), "second lease applied");
    sleep(12); /* server silent: renew, rebind, expiry */
    CHECK(inet_line(line, sizeof line) && strstr(line, " 0.0.0.0/"), "expiry removes the address");
    kill(pid, SIGTERM);
    wait_status(pid);
    close(out);

    /* 6. The offered 10.0.2.2 answers ARP, so the client sends DHCPDECLINE
     * and discovers again after the shortened wait. */
    char output[1024];
    const char *const acd[] = {"/bin/dhcpc", "-1", "-t", "30", "-A", "-l", "/tmp/dhcp-acd.lease",
                               "-s", "127.0.0.1", "-p", "6767", NULL};
    drain();
    pid = spawn(acd, &out);
    n = await(m, 1, 5000, &from);
    if (n)
        reply(m, n, &from, 2, 0x0a000202, 0, 1, 600, 0, 0);
    n = await(m, 3, 5000, &from);
    if (n)
        reply(m, n, &from, 5, 0x0a000202, 0, 1, 600, 0, 0);
    n = await(m, 4, 5000, &from);
    CHECK(n && option32(m, n, 50) == 0x0a000202 && option32(m, n, 54) == 0x0a000202 &&
          !get32(m + 12) && !has_option(m, n, 55), "DHCPDECLINE names the address and the server");
    n = await(m, 1, 5000, &from);
    CHECK(n && !option32(m, n, 50), "discovery again after the decline");
    if (n)
        reply(m, n, &from, 2, 0x0a00020f, 0, 1, 600, 0, 0);
    n = await(m, 3, 5000, &from);
    if (n)
        reply(m, n, &from, 5, 0x0a00020f, 0, 1, 600, 0, 0);
    CHECK(wait_status(pid) == 0, "lease after the conflict");
    memset(output, 0, sizeof output);
    read(out, output, sizeof output - 1);
    close(out);
    CHECK(strstr(output, "address 10.0.2.2 in use by 52:55:0a:00:02:02, declined") != NULL,
          "conflict reported with the other host's address");
    CHECK(strstr(output, "bound 10.0.2.15") != NULL && file_has("/tmp/dhcp-acd.lease", "address 10.0.2.15"),
          "second address bound and saved");

    /* 7. In INIT-REBOOT the saved lease is requested again without a server
     * identifier, and the server acknowledges it. */
    drain();
    pid = spawn(acd, &out);
    n = await(m, 3, 5000, &from);
    CHECK(n && option32(m, n, 50) == 0x0a00020f && !option32(m, n, 54) && !get32(m + 12),
          "INIT-REBOOT request carries the saved address only");
    if (n)
        reply(m, n, &from, 5, 0x0a00020f, 0, 1, 600, 0, 0);
    CHECK(wait_status(pid) == 0, "rebooted lease");
    memset(output, 0, sizeof output);
    read(out, output, sizeof output - 1);
    close(out);
    CHECK(strstr(output, "rebooting with 10.0.2.15") && strstr(output, "bound 10.0.2.15"),
          "INIT-REBOOT reported");

    /* 8. After a NAK for the saved address the file is removed, and
     * discovery starts without asking for that address. */
    drain();
    pid = spawn(acd, &out);
    n = await(m, 3, 5000, &from);
    if (n)
        reply(m, n, &from, 6, 0, 0, 1, 0, 0, 0);
    n = await(m, 1, 5000, &from);
    CHECK(n && !option32(m, n, 50) && !file_has("/tmp/dhcp-acd.lease", "address"),
          "NAK forgets the saved lease");
    if (n)
        reply(m, n, &from, 2, 0x0a00020f, 0, 1, 600, 0, 0);
    n = await(m, 3, 5000, &from);
    if (n)
        reply(m, n, &from, 5, 0x0a00020f, 0, 1, 600, 0, 0);
    CHECK(wait_status(pid) == 0, "lease after the NAK");
    close(out);

    /* 9. An expired saved lease is not requested again. */
    FILE *old = fopen("/tmp/dhcp-old.lease", "w");
    fprintf(old, "address 10.0.2.15\nserver 10.0.2.2\nexpires 1\n");
    fclose(old);
    const char *const expired[] = {"/bin/dhcpc", "-1", "-t", "30", "-A", "-l", "/tmp/dhcp-old.lease",
                                   "-s", "127.0.0.1", "-p", "6767", NULL};
    drain();
    pid = spawn(expired, &out);
    n = await(m, -1, 5000, &from);
    CHECK(n && option(m, n, 53) == 1, "an expired lease starts with discovery");
    kill(pid, SIGTERM);
    wait_status(pid);
    close(out);
    close(server_fd);

    /* 5. Live lease from QEMU's user-mode server over the NIC. */
    const char *const live[] = {"/bin/dhcpc", "-1", "-t", "30", NULL};
    pid = spawn(live, &out);
    CHECK(wait_status(pid) == 0, "live lease acquired");
    memset(text, 0, sizeof text);
    read(out, text, sizeof text - 1);
    close(out);
    CHECK(strstr(text, "bound 10.0.2.15 from 10.0.2.2") != NULL, "live lease reported");
    CHECK(inet_line(line, sizeof line) && strstr(line, "10.0.2.15/255.255.255.0 gw 10.0.2.2"),
          "live lease applied");
    const char *const saved = "/home/.local/state/dhcpc/eth0.lease";
    CHECK(file_has(saved, "address 10.0.2.15") && file_has(saved, "server 10.0.2.2"),
          "live lease saved on the home volume");

    /* 10. The next start requests the saved address from QEMU's server. */
    pid = spawn(live, &out);
    CHECK(wait_status(pid) == 0, "live INIT-REBOOT");
    memset(output, 0, sizeof output);
    read(out, output, sizeof output - 1);
    close(out);
    CHECK(strstr(output, "rebooting with 10.0.2.15") && strstr(output, "bound 10.0.2.15 from 10.0.2.2"),
          "QEMU acknowledges the saved address");
    printf("netdhcptest: %d failures\n", failures);
    return failures ? 1 : 0;
}
