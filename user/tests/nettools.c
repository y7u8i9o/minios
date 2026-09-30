/* N10/N11 tools: net config, ping through /dev/net, nc and http on loopback.
 * Runs with nic user so that 10.0.2.2 answers echo requests. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <pthread.h>
#include <sys/wait.h>
#include <sys/ipc.h>
#include <sys/socket.h>
#include <netinet/in.h>

static int failures;
#define CHECK(c, text) do { if (!(c)) { printf("nettools: FAIL %s (errno %d)\n", text, errno); failures++; } } while (0)

/* Run a program with optional stdin text; return its exit status and output. */
static int run(const char *const *args, const char *input, char *output, size_t size)
{
    int in[2], out[2];
    pipe(in);
    pipe(out);
    pid_t pid = fork();
    if (pid == 0) {
        dup2(in[0], 0);
        dup2(out[1], 1);
        close(in[0]); close(in[1]); close(out[0]); close(out[1]);
        execv(args[0], (char *const *)args);
        _exit(127);
    }
    close(in[0]);
    close(out[1]);
    if (input)
        write(in[1], input, strlen(input));
    close(in[1]);
    size_t got = 0;
    ssize_t n;
    while (got + 1 < size && (n = read(out[0], output + got, size - 1 - got)) > 0)
        got += (size_t)n;
    output[got] = 0;
    close(out[0]);
    int status = -1;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static void *http_server(void *arg)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in name = {.sin_family = AF_INET, .sin_port = htons(8080),
                               .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    bind(fd, (struct sockaddr *)&name, sizeof name);
    listen(fd, 2);
    for (int i = 0; i < 3; i++) {
        int c = accept(fd, NULL, NULL);
        char request[1024];
        ssize_t n = read(c, request, sizeof request - 1);
        request[n > 0 ? n : 0] = 0;
        const char *response = strstr(request, "GET /hello ")
            ? "HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\n\r\nhello from minios\n"
            : "HTTP/1.0 404 Not Found\r\n\r\nmissing\n";
        write(c, response, strlen(response));
        close(c);
    }
    close(fd);
    return NULL;
}

static void *udp_echo(void *arg)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in name = {.sin_family = AF_INET, .sin_port = htons(8081),
                               .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    bind(fd, (struct sockaddr *)&name, sizeof name);
    char buf[256];
    struct sockaddr_in from;
    socklen_t len = sizeof from;
    ssize_t n = recvfrom(fd, buf, sizeof buf, 0, (struct sockaddr *)&from, &len);
    if (n > 0)
        sendto(fd, buf, (size_t)n, 0, (struct sockaddr *)&from, len);
    close(fd);
    return NULL;
}

int main(void)
{
    char out[4096];
    const char *const bad[] = {"/bin/net", "config", "eth0", "10.0.2.15", "255.255.255.0", "10.0.3.1", NULL};
    CHECK(run(bad, NULL, out, sizeof out) == 1, "gateway outside the subnet rejected");
    const char *const config[] = {"/bin/net", "config", "eth0", "10.0.2.15", "255.255.255.0", "10.0.2.2", NULL};
    CHECK(run(config, NULL, out, sizeof out) == 0, "static configuration");
    /* net apply writes the name servers and the search list (N15). The
     * files are restored afterwards, because the later checks expect no
     * resolver and no check may reach a public name server. */
    char saved[512];
    FILE *network = fopen("/etc/network", "r");
    size_t saved_length = network ? fread(saved, 1, sizeof saved, network) : 0;
    if (network)
        fclose(network);
    network = fopen("/etc/network", "w");
    fprintf(network, "iface eth0 static 10.0.2.15 255.255.255.0 10.0.2.2\n"
                     "nameserver 127.0.0.1\nsearch example.test other.test\n");
    fclose(network);
    const char *const apply[] = {"/bin/net", "apply", NULL};
    CHECK(run(apply, NULL, out, sizeof out) == 0, "net apply");
    FILE *resolv = fopen("/etc/resolv.conf", "r");
    size_t length = resolv ? fread(out, 1, sizeof out - 1, resolv) : 0;
    out[length] = 0;
    if (resolv)
        fclose(resolv);
    CHECK(strstr(out, "nameserver 127.0.0.1\n") && strstr(out, "search example.test other.test\n"),
          "net apply writes the search list");
    unlink("/etc/resolv.conf");
    network = fopen("/etc/network", "w");
    fwrite(saved, 1, saved_length, network);
    fclose(network);
    const char *const show[] = {"/bin/net", NULL};
    CHECK(run(show, NULL, out, sizeof out) == 0 && strstr(out, "inet eth0 10.0.2.15/255.255.255.0 gw 10.0.2.2") &&
          strstr(out, "link eth0 ") && strstr(out, "tcp active"), "net shows the configuration and counters");

    const char *const ping[] = {"/bin/ping", "-c", "2", "10.0.2.2", NULL};
    CHECK(run(ping, NULL, out, sizeof out) == 0 && strstr(out, "2 packets transmitted, 2 received"),
          "ping reaches the gateway");
    const char *const ping_fail[] = {"/bin/ping", "-c", "1", "-W", "500", "10.0.2.99", NULL};
    CHECK(run(ping_fail, NULL, out, sizeof out) == 1 && strstr(out, "1 packets transmitted, 0 received"),
          "ping reports an unreachable host");
    const char *const ping_name[] = {"/bin/ping", "-c", "1", "no-such-host.invalid", NULL};
    CHECK(run(ping_name, NULL, out, sizeof out) == 1 && strstr(out, "") , "ping without resolver fails");

    pthread_t http, udp;
    pthread_create(&http, NULL, http_server, NULL);
    pthread_create(&udp, NULL, udp_echo, NULL);
    usleep(100000);
    const char *const get[] = {"/bin/http", "http://127.0.0.1:8080/hello", NULL};
    CHECK(run(get, NULL, out, sizeof out) == 0 && strcmp(out, "hello from minios\n") == 0, "http GET body");
    const char *const missing[] = {"/bin/http", "http://localhost:8080/missing", NULL};
    CHECK(run(missing, NULL, out, sizeof out) == 1 && strcmp(out, "missing\n") == 0, "http reports 404");
    const char *const https[] = {"/bin/http", "https://127.0.0.1/", NULL};
    CHECK(run(https, NULL, out, sizeof out) == 2, "https refused");

    const char *const nc_tcp[] = {"/bin/nc", "127.0.0.1", "8080", NULL};
    CHECK(run(nc_tcp, "GET /hello HTTP/1.0\r\n\r\n", out, sizeof out) == 0 &&
          strstr(out, "hello from minios"), "nc TCP client relays both directions");
    const char *const nc_udp[] = {"/bin/nc", "-u", "127.0.0.1", "8081", NULL};
    /* The UDP relay has no EOF; send, wait for the echo, then stop it. */
    int in[2], outp[2];
    pipe(in); pipe(outp);
    pid_t pid = fork();
    if (pid == 0) {
        dup2(in[0], 0); dup2(outp[1], 1);
        close(in[0]); close(in[1]); close(outp[0]); close(outp[1]);
        execv(nc_udp[0], (char *const *)nc_udp);
        _exit(127);
    }
    close(in[0]); close(outp[1]);
    write(in[1], "ping udp\n", 9);
    char echo[64] = "";
    struct pollfd pfd = {.fd = outp[0], .events = POLLIN};
    if (poll(&pfd, 1, 3000) == 1)
        read(outp[0], echo, sizeof echo - 1);
    CHECK(strcmp(echo, "ping udp\n") == 0, "nc UDP client echo");
    kill(pid, SIGTERM);
    waitpid(pid, NULL, 0);
    close(in[1]); close(outp[0]);
    const char *const nc_bad[] = {"/bin/nc", "127.0.0.1", "1", NULL};
    CHECK(run(nc_bad, NULL, out, sizeof out) == 1, "nc reports a refused connection");
    pthread_join(http, NULL);
    pthread_join(udp, NULL);
    printf("nettools: %d failures\n", failures);
    return failures ? 1 : 0;
}
