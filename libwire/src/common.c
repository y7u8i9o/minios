/* Wire format: marshalling into the output buffer, socket I/O with
 * descriptor records, message framing and decoding. */
#include <wire/common.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <fcntl.h>

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static void wr32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static void wr16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static size_t pad4(size_t n) { return (n + 3) & ~(size_t)3; }

void wire_conn_init(struct wire_conn *c, int fd)
{
    memset(c, 0, sizeof *c);
    c->fd = fd;
}

void wire_conn_close(struct wire_conn *c)
{
    for (int i = 0; i < c->nout_fds; i++)
        close(c->out_fds[i]);
    for (int i = 0; i < c->nin_fds; i++)
        close(c->in_fds[i]);
    c->nout_fds = c->nin_fds = 0;
    if (c->fd >= 0)
        close(c->fd);
    c->fd = -1;
}

static size_t message_size(const struct wire_message *m, const union wire_arg *args)
{
    size_t n = 8;
    const char *sig = m->signature;
    for (int i = 0; i < m->nargs; i++) {
        if (*sig == '?')
            sig++;
        switch (*sig++) {
        case 's': n += 4 + pad4(args[i].s ? strlen(args[i].s) + 1 : 0); break;
        case 'a': n += 4 + pad4(args[i].a ? args[i].a->size : 0); break;
        case 'h': break;
        default: n += 4; break;
        }
    }
    return n;
}

int wire_conn_marshal(struct wire_conn *c, uint32_t id, uint32_t opcode, const struct wire_message *m,
                      const union wire_arg *args)
{
    size_t size = message_size(m, args);
    int nfds = 0;
    for (const char *s = m->signature; *s; s++)
        nfds += *s == 'h';
    if (size > WIRE_MAX_MESSAGE)
        return -1;
    if (c->out_len + size > sizeof c->out || c->nout_fds + nfds > 16) {
        if (wire_conn_flush(c) < 0)
            return -1;
        /* The peer is not reading: the message is dropped, the
         * connection kept (the server shows the client as not responding). */
        if (c->out_len + size > sizeof c->out || c->nout_fds + nfds > 16)
            return -1;
    }
    uint8_t *p = c->out + c->out_len;
    wr32(p, id);
    wr16(p + 4, opcode);
    wr16(p + 6, (uint32_t)size);
    p += 8;
    const char *sig = m->signature;
    for (int i = 0; i < m->nargs; i++) {
        if (*sig == '?')
            sig++;
        switch (*sig++) {
        case 'i': case 'f': wr32(p, (uint32_t)args[i].i); p += 4; break;
        case 'u': wr32(p, args[i].u); p += 4; break;
        case 'o': wr32(p, args[i].o); p += 4; break;
        case 'n': wr32(p, args[i].n); p += 4; break;
        case 's': {
            size_t len = args[i].s ? strlen(args[i].s) + 1 : 0;
            wr32(p, (uint32_t)len);
            p += 4;
            if (len)
                memcpy(p, args[i].s, len);
            memset(p + len, 0, pad4(len) - len);
            p += pad4(len);
            break;
        }
        case 'a': {
            size_t len = args[i].a ? args[i].a->size : 0;
            wr32(p, (uint32_t)len);
            p += 4;
            if (len)
                memcpy(p, args[i].a->data, len);
            memset(p + len, 0, pad4(len) - len);
            p += pad4(len);
            break;
        }
        case 'h': {
            int dup_fd = fcntl(args[i].h, F_DUPFD_CLOEXEC, 0);
            if (dup_fd < 0)
                return -1;
            c->out_fds[c->nout_fds++] = dup_fd;
            break;
        }
        }
    }
    c->out_len += size;
    return 0;
}

int wire_conn_flush(struct wire_conn *c)
{
    if (c->error)
        return -1;
    size_t off = 0;
    while (off < c->out_len) {
        struct iovec iov = { c->out + off, c->out_len - off };
        char ctl[CMSG_SPACE(sizeof(int) * 16)];
        struct msghdr m = { NULL, 0, &iov, 1, NULL, 0, 0 };
        int nfds = c->nout_fds > 16 ? 16 : c->nout_fds;
        if (nfds) {
            m.msg_control = ctl;
            m.msg_controllen = CMSG_SPACE(sizeof(int) * (size_t)nfds);
            struct cmsghdr *h = CMSG_FIRSTHDR(&m);
            h->cmsg_level = SOL_SOCKET;
            h->cmsg_type = SCM_RIGHTS;
            h->cmsg_len = CMSG_LEN(sizeof(int) * (size_t)nfds);
            memcpy(CMSG_DATA(h), c->out_fds, sizeof(int) * (size_t)nfds);
        }
        ssize_t n = sendmsg(c->fd, &m, 0);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN)
                break;
            fprintf(stderr, "wire: send of %zu bytes and %d descriptors failed: %s\n", c->out_len - off, nfds,
                    strerror(errno));
            c->error = 1;
            return -1;
        }
        if (nfds) {
            for (int i = 0; i < nfds; i++)
                close(c->out_fds[i]);
            memmove(c->out_fds, c->out_fds + nfds, sizeof(int) * (size_t)(c->nout_fds - nfds));
            c->nout_fds -= nfds;
        }
        off += (size_t)n;
    }
    memmove(c->out, c->out + off, c->out_len - off);
    c->out_len -= off;
    return 0;
}

int wire_conn_read(struct wire_conn *c)
{
    if (c->error)
        return -1;
    if (c->in_len == sizeof c->in)
        return -1;
    struct iovec iov = { c->in + c->in_len, sizeof c->in - c->in_len };
    char ctl[CMSG_SPACE(sizeof(int) * 16)];
    struct msghdr m = { NULL, 0, &iov, 1, ctl, sizeof ctl, 0 };
    ssize_t n;
    do {
        n = recvmsg(c->fd, &m, 0);
    } while (n < 0 && errno == EINTR);
    if (n < 0) {
        if (errno == EAGAIN)
            return 0;
        c->error = 1;
        return -1;
    }
    for (struct cmsghdr *h = CMSG_FIRSTHDR(&m); h; h = CMSG_NXTHDR(&m, h)) {
        if (h->cmsg_level != SOL_SOCKET || h->cmsg_type != SCM_RIGHTS)
            continue;
        int k = (int)((h->cmsg_len - CMSG_LEN(0)) / sizeof(int));
        const int *fds = (const int *)CMSG_DATA(h);
        for (int i = 0; i < k; i++) {
            if (c->nin_fds < WIRE_MAX_FDS)
                c->in_fds[c->nin_fds++] = fds[i];
            else
                close(fds[i]);
        }
    }
    if (n == 0) {
        c->error = 1;
        return 0;
    }
    c->in_len += (size_t)n;
    return (int)n;
}

int wire_conn_peek(struct wire_conn *c, uint32_t *id, uint32_t *opcode, const uint8_t **body, size_t *len)
{
    if (c->in_len < 8)
        return 0;
    uint32_t size = c->in[6] | (uint32_t)c->in[7] << 8;
    if (size < 8) {
        c->error = 1;
        return -1;
    }
    if (c->in_len < size)
        return 0;
    *id = rd32(c->in);
    *opcode = c->in[4] | (uint32_t)c->in[5] << 8;
    *body = c->in + 8;
    *len = size - 8;
    return 1;
}

void wire_conn_consume(struct wire_conn *c, size_t len)
{
    size_t size = len + 8;
    memmove(c->in, c->in + size, c->in_len - size);
    c->in_len -= size;
}

int wire_unmarshal(struct wire_conn *c, const struct wire_message *m, const uint8_t *body, size_t len,
                   union wire_arg *args, struct wire_array *arrays)
{
    size_t off = 0;
    const char *sig = m->signature;
    for (int i = 0; i < m->nargs; i++) {
        int nullable = 0;
        if (*sig == '?') {
            nullable = 1;
            sig++;
        }
        char t = *sig++;
        if (t != 'h' && off + 4 > len)
            return -1;
        switch (t) {
        case 'i': case 'f': args[i].i = (int32_t)rd32(body + off); off += 4; break;
        case 'u': args[i].u = rd32(body + off); off += 4; break;
        case 'o': args[i].o = rd32(body + off); off += 4; if (!args[i].o && !nullable) return -1; break;
        case 'n': args[i].n = rd32(body + off); off += 4; break;
        case 's': {
            uint32_t slen = rd32(body + off);
            off += 4;
            if (off + pad4(slen) > len || (slen && body[off + slen - 1] != '\0'))
                return -1;
            args[i].s = slen ? (const char *)(body + off) : NULL;
            if (!args[i].s && !nullable)
                return -1;
            off += pad4(slen);
            break;
        }
        case 'a': {
            uint32_t alen = rd32(body + off);
            off += 4;
            if (off + pad4(alen) > len)
                return -1;
            arrays[i].data = body + off;
            arrays[i].size = alen;
            args[i].a = &arrays[i];
            off += pad4(alen);
            break;
        }
        case 'h':
            if (c->nin_fds == 0)
                return -1;
            args[i].h = c->in_fds[0];
            memmove(c->in_fds, c->in_fds + 1, sizeof(int) * (size_t)(c->nin_fds - 1));
            c->nin_fds--;
            break;
        default:
            return -1;
        }
    }
    return off == len ? 0 : -1;
}
