#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

int inet_aton(const char *text, struct in_addr *out)
{
    unsigned parts[4];
    int n = 0;
    const char *p = text;
    while (n < 4) {
        if (*p < '0' || *p > '9')
            return 0;
        unsigned v = 0;
        while (*p >= '0' && *p <= '9') {
            v = v * 10 + (unsigned)(*p++ - '0');
            if (v > 255)
                return 0;
        }
        parts[n++] = v;
        if (*p != '.')
            break;
        p++;
    }
    if (n != 4 || *p)
        return 0;
    out->s_addr = htonl(parts[0] << 24 | parts[1] << 16 | parts[2] << 8 | parts[3]);
    return 1;
}

in_addr_t inet_addr(const char *text)
{
    struct in_addr in;
    return inet_aton(text, &in) ? in.s_addr : INADDR_NONE;
}

char *inet_ntoa(struct in_addr in)
{
    static char text[16];
    inet_ntop(AF_INET, &in, text, sizeof text);
    return text;
}

int inet_pton(int family, const char *text, void *out)
{
    if (family != AF_INET) {
        errno = EAFNOSUPPORT;
        return -1;
    }
    return inet_aton(text, out);
}

const char *inet_ntop(int family, const void *in, char *out, socklen_t size)
{
    if (family != AF_INET) {
        errno = EAFNOSUPPORT;
        return NULL;
    }
    uint32_t a = ntohl(((const struct in_addr *)in)->s_addr);
    char text[16];
    snprintf(text, sizeof text, "%u.%u.%u.%u", a >> 24, (a >> 16) & 255, (a >> 8) & 255, a & 255);
    if (strlen(text) >= size) {
        errno = ENOSPC;
        return NULL;
    }
    strcpy(out, text);
    return out;
}
