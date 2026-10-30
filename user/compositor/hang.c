/* Unresponsive clients. The server pings every client through its shell
 * global once a second. A client that has not answered for three seconds,
 * or whose socket has remained full that long, counts as not responding.
 * The window manager module dims the toplevels of the client (wm.h). The
 * session manager receives the state of the client and shows the dialog
 * with Wait and Force quit (session.c, docs/design/sessiond.md). A client
 * that answers again gets its windows back unchanged. */
#include <stdio.h>
#include <unistd.h>
#include "comp.h"

#define PING_INTERVAL 1000
#define HANG_AFTER    3000

static struct wire_server *server;

void hang_init(struct wire_server *srv)
{
    server = srv;
}

void hang_client_attached(struct client *c)
{
    c->last_pong = uptime_ms();
}

static void damage_client(struct client *c)
{
    for (struct csurface *s = surface_first(); s; s = s->next)
        if (s->client == c && s->role == ROLE_TOPLEVEL)
            scene_damage((struct rect){ s->x, s->y, s->width, s->height });
}

void hang_tick(long now)
{
    for (struct wire_client *wc = wire_server_first_client(server); wc; wc = wire_client_next(wc)) {
        struct client *c = wire_client_get_user_data(wc);
        if (!c || !c->shell_res)
            continue;
        if (now - c->ping_sent >= PING_INTERVAL) {
            c->ping_serial++;
            shell_send_ping(c->shell_res, c->ping_serial);
            c->ping_sent = now;
        }
        int hung = now - c->last_pong >= HANG_AFTER || (c->stall_since && now - c->stall_since >= HANG_AFTER);
        if (hung != c->unresponsive) {
            c->unresponsive = hung;
            comp_log("client %d %s", c->number, hung ? "not responding" : "responding again");
            damage_client(c);
            session_client_state(c, !hung);
        }
    }
}

/* The earliest of: the next ping of a client, and the moment at which a
 * responsive client counts as hung without a pong or with a full socket.
 * A pong or a socket that drains changes the state during the dispatch
 * that brings them, and hang_tick runs after every dispatch. */
long hang_next_deadline(void)
{
    long next = -1;
    for (struct wire_client *wc = wire_server_first_client(server); wc; wc = wire_client_next(wc)) {
        struct client *c = wire_client_get_user_data(wc);
        if (!c || !c->shell_res)
            continue;
        long t[3] = { c->ping_sent + PING_INTERVAL, c->unresponsive ? -1 : c->last_pong + HANG_AFTER,
                      c->unresponsive || !c->stall_since ? -1 : c->stall_since + HANG_AFTER };
        for (int i = 0; i < 3; i++)
            if (t[i] >= 0 && (next < 0 || t[i] < next))
                next = t[i];
    }
    return next;
}
