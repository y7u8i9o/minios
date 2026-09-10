/* AF_INET sockets: type and protocol validation and the protocol table.
 * inet_protocols_lock protects the table; it is a leaf. */
#include <net/inet.h>
#include <errno.h>

#define INET_MAX_PROTOCOLS 4

static const struct inet_protocol *protocols[INET_MAX_PROTOCOLS];
static DEFINE_SPINLOCK(inet_protocols_lock);

int inet_register_protocol(const struct inet_protocol *p)
{
    spin_lock(&inet_protocols_lock);
    for (int i = 0; i < INET_MAX_PROTOCOLS; i++) {
        if (!protocols[i]) {
            protocols[i] = p;
            spin_unlock(&inet_protocols_lock);
            return 0;
        }
    }
    spin_unlock(&inet_protocols_lock);
    return -ENOSPC;
}

/* The default protocol of a type, as socket(AF_INET, type, 0) selects. */
static int default_protocol(int type)
{
    switch (type) {
    case SOCK_STREAM:
        return IPPROTO_TCP;
    case SOCK_DGRAM:
        return IPPROTO_UDP;
    default:
        return -1;
    }
}

static int inet_create(struct socket *s)
{
    int proto = s->protocol == 0 ? default_protocol(s->type) : s->protocol;
    if (s->type != SOCK_STREAM && s->type != SOCK_DGRAM)
        return -ESOCKTNOSUPPORT;
    if ((s->type == SOCK_STREAM && proto != IPPROTO_TCP) ||
        (s->type == SOCK_DGRAM && proto != IPPROTO_UDP))
        return -EPROTONOSUPPORT;
    const struct inet_protocol *p = NULL;
    spin_lock(&inet_protocols_lock);
    for (int i = 0; i < INET_MAX_PROTOCOLS; i++)
        if (protocols[i] && protocols[i]->type == s->type && protocols[i]->protocol == proto)
            p = protocols[i];
    spin_unlock(&inet_protocols_lock);
    if (!p)
        return -EPROTONOSUPPORT;
    s->protocol = proto;
    return p->create(s);
}

static const struct socket_family inet_family = {
    .family = AF_INET,
    .create = inet_create,
};

void inet_socket_init(void)
{
    socket_register_family(&inet_family);
}
