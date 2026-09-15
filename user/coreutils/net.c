/* net: interface configuration and diagnostics through /dev/net (N10).
 *   net                     show interfaces, addresses, neighbours, counters
 *   net config IF ADDR MASK [GATEWAY]
 *   net down IF             remove the address, mask and gateway
 *   net apply               apply /etc/network (static, or start dhcpc)
 * MiniOS is single user; every process may configure the network. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <arpa/inet.h>
#include <minios/abi.h>

static int usage(void)
{
    fprintf(stderr, "usage: net [config IF ADDR MASK [GW] | down IF | apply]\n");
    return 2;
}

static int configure(const char *name, uint32_t address, uint32_t mask, uint32_t gateway)
{
    int fd = open("/dev/net", O_RDONLY);
    if (fd < 0) {
        perror("net: /dev/net");
        return 1;
    }
    struct net_config c = {.address = address, .mask = mask, .gateway = gateway};
    strncpy(c.name, name, sizeof c.name - 1);
    int r = ioctl(fd, NETIOC_CONFIGURE, &c);
    close(fd);
    if (r < 0) {
        fprintf(stderr, "net: configure %s: %s\n", name, strerror(errno));
        return 1;
    }
    return 0;
}

static int parse(const char *text, uint32_t *out)
{
    struct in_addr in;
    if (!inet_aton(text, &in)) {
        fprintf(stderr, "net: invalid address %s\n", text);
        return 0;
    }
    *out = ntohl(in.s_addr);
    return 1;
}

static int show(void)
{
    FILE *f = fopen("/dev/net", "r");
    if (!f) {
        perror("net: /dev/net");
        return 1;
    }
    char line[512];
    while (fgets(line, sizeof line, f))
        fputs(line, stdout);
    fclose(f);
    return 0;
}

static int has_interface(const char *name)
{
    FILE *f = fopen("/dev/net", "r");
    if (!f)
        return 0;
    char line[512], found[64];
    int ok = 0;
    while (!ok && fgets(line, sizeof line, f)) {
        if (sscanf(line, "link %63s", found) == 1 && strcmp(found, name) == 0)
            ok = 1;
    }
    fclose(f);
    return ok;
}

/* /etc/network: "iface NAME dhcp" or "iface NAME static ADDR MASK [GW]",
 * plus "nameserver ADDR" lines written to /etc/resolv.conf for static
 * configurations. Missing file or interface means no network. */
static int apply(void)
{
    FILE *f = fopen("/etc/network", "r");
    if (!f)
        return 0;
    char line[256];
    int status = 0;
    FILE *resolv = NULL;
    while (fgets(line, sizeof line, f)) {
        char *hash = strchr(line, '#');
        if (hash)
            *hash = 0;
        char *words[8];
        int n = 0;
        const char *save;
        for (char *w = strtok_r(line, " \t\r\n", &save); w && n < 8; w = strtok_r(NULL, " \t\r\n", &save))
            words[n++] = w;
        if (n >= 2 && strcmp(words[0], "nameserver") == 0) {
            if (!resolv)
                resolv = fopen("/etc/resolv.conf", "w");
            if (resolv)
                fprintf(resolv, "nameserver %s\n", words[1]);
            continue;
        }
        if (n < 3 || strcmp(words[0], "iface") != 0 || !has_interface(words[1]))
            continue;
        if (strcmp(words[2], "dhcp") == 0) {
            pid_t pid = fork();
            if (pid == 0) {
                execl("/bin/dhcpc", "dhcpc", "-i", words[1], (char *)NULL);
                _exit(127);
            }
            if (pid < 0)
                status = 1;
        } else if (strcmp(words[2], "static") == 0 && n >= 5) {
            uint32_t a, m, g = 0;
            if (parse(words[3], &a) && parse(words[4], &m) && (n < 6 || parse(words[5], &g)))
                status |= configure(words[1], a, m, g);
            else
                status = 1;
        } else {
            fprintf(stderr, "net: bad /etc/network line for %s\n", words[1]);
            status = 1;
        }
    }
    if (resolv)
        fclose(resolv);
    fclose(f);
    return status;
}

int main(int argc, char **argv)
{
    if (argc == 1)
        return show();
    if (strcmp(argv[1], "apply") == 0)
        return apply();
    if (strcmp(argv[1], "down") == 0 && argc == 3)
        return configure(argv[2], 0, 0, 0);
    if (strcmp(argv[1], "config") == 0 && (argc == 5 || argc == 6)) {
        uint32_t a, m, g = 0;
        if (!parse(argv[3], &a) || !parse(argv[4], &m) || (argc == 6 && !parse(argv[5], &g)))
            return 1;
        return configure(argv[2], a, m, g);
    }
    return usage();
}
