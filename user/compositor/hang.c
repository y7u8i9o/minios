/* Unresponsive clients. The server pings every client through its shell
 * global once a second; a client that has not answered for three seconds
 * (or whose socket has stayed full that long) is shown as not responding:
 * its toplevels are dimmed and carry a dialog with the title, Wait and
 * Force quit. Wait hides the dialog for fifteen seconds, Force quit kills
 * the process (when the client reported its pid) and drops the connection.
 * A client that answers again gets its windows back unchanged. */
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <gui/paint.h>
#include "comp.h"

#define PING_INTERVAL 1000
#define HANG_AFTER    3000
#define SNOOZE        15000
#define BOX_W 320
#define BOX_H 96
#define BUTTON_W 96
#define BUTTON_H 26
#define COLOR_DIM    0x00404040
#define COLOR_BOX    0x00f4f5f7
#define COLOR_EDGE   0x00a4aab3
#define COLOR_TEXT   0x0025282d
#define COLOR_BUTTON 0x00dcdfe3
#define COLOR_QUIT   0x00e5484d
#define COLOR_QUIT_TEXT 0x00ffffff

static struct wire_server *server;

void hang_init(struct wire_server *srv)
{
    server = srv;
}

void hang_client_attached(struct client *c)
{
    c->last_pong = uptime_ms();
}

static int shown(const struct client *c, long now)
{
    return c->unresponsive && now >= c->snooze_until;
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
            if (!hung)
                c->snooze_until = 0;
            comp_log("client %d %s", c->number, hung ? "not responding" : "responding again");
            damage_client(c);
        } else if (hung && c->snooze_until && now >= c->snooze_until) {
            c->snooze_until = 0;
            damage_client(c);
        }
    }
}

/* The dialog box, centred in the toplevel (logical coordinates). */
static struct rect box_rect(const struct csurface *s)
{
    int w = BOX_W < s->width - 16 ? BOX_W : s->width - 16, h = BOX_H < s->height - 16 ? BOX_H : s->height - 16;
    return (struct rect){ s->x + (s->width - w) / 2, s->y + (s->height - h) / 2, w, h };
}

/* n: 0 Wait, 1 Force quit. Two buttons at the bottom right; in a narrow
 * window they shrink so both fit. */
static struct rect button_rect(struct rect box, int n)
{
    int bw = (box.w - 24 - 8) / 2;
    if (bw > BUTTON_W)
        bw = BUTTON_W;
    int x = box.x + box.w - 12 - bw - (n == 0 ? bw + 8 : 0);
    return (struct rect){ x, box.y + box.h - 12 - BUTTON_H, bw, BUTTON_H };
}

static void draw_button(struct painter *p, struct rect b, int ox, int oy, const char *label, uint32_t fill, uint32_t fg)
{
    painter_rounded(p, b.x + ox, b.y + oy, b.w, b.h, fill, COLOR_EDGE);
    const struct font *f = decor_font();
    int S = screen_scale;
    int tw = (gfx_text_width_font_scaled(f, label, -1, S) + S - 1) / S;
    painter_text_font(p, f, b.x + ox + (b.w - tw) / 2, b.y + oy + (b.h - f->height) / 2, label, fg, 0xffffffffu);
}

void hang_draw(struct csurface *s, struct rect clip)
{
    if (s->role != ROLE_TOPLEVEL || !s->toplevel || !shown(s->client, uptime_ms()))
        return;
    struct rect win = { s->x, s->y, s->width, s->height };
    struct rect c = rect_intersect(win, clip);
    if (rect_empty(c))
        return;
    int S = screen_scale;
    /* Dim the window contents. */
    struct rect d = rect_intersect((struct rect){ c.x * S, c.y * S, c.w * S, c.h * S },
                                   (struct rect){ 0, 0, back.width, back.height });
    for (int y = d.y; y < d.y + d.h; y++) {
        uint32_t *row = &back.pixels[(size_t)y * back.stride];
        for (int x = d.x; x < d.x + d.w; x++)
            row[x] = ((row[x] >> 1) & 0x007f7f7f) + (COLOR_DIM >> 1);
    }
    struct rect box = box_rect(s);
    if (rect_empty(rect_intersect(box, c)))
        return;
    struct painter p;
    painter_init_scaled(&p, &back, decor_theme_ptr(), S);
    painter_push(&p, c.x, c.y, c.w, c.h);
    int ox = -c.x, oy = -c.y;
    painter_rounded(&p, box.x + ox, box.y + oy, box.w, box.h, COLOR_BOX, COLOR_EDGE);
    char text[96];
    snprintf(text, sizeof text, "%s is not responding", s->toplevel->title[0] ? s->toplevel->title : "The window");
    painter_push(&p, box.x + 16 + ox, box.y + 14 + oy, box.w - 32, decor_font()->height + 2);
    painter_text_font(&p, decor_font(), 0, 0, text, COLOR_TEXT, 0xffffffffu);
    painter_pop(&p);
    draw_button(&p, button_rect(box, 0), ox, oy, "Wait", COLOR_BUTTON, COLOR_TEXT);
    draw_button(&p, button_rect(box, 1), ox, oy, "Force quit", COLOR_QUIT, COLOR_QUIT_TEXT);
    painter_pop(&p);
}

int hang_press(int x, int y)
{
    long now = uptime_ms();
    struct csurface *hit = NULL;
    for (struct csurface *s = surface_first(); s; s = s->next) {
        if (s->role != ROLE_TOPLEVEL || !s->toplevel || s->toplevel->minimized || !shown(s->client, now))
            continue;
        if (x < s->x || y < s->y || x >= s->x + s->width || y >= s->y + s->height)
            continue;
        if (!hit || s->stack > hit->stack)
            hit = s;
    }
    if (!hit)
        return 0;
    struct csurface *top = scene_surface_at(x, y);
    if (top && top != hit && top->stack > hit->stack)
        return 0;                       /* another window covers the spot */
    struct rect box = box_rect(hit);
    struct client *c = hit->client;
    if (rect_contains(button_rect(box, 0), x, y)) {
        c->snooze_until = now + SNOOZE;
        comp_log("client %d: wait", c->number);
        damage_client(c);
    } else if (rect_contains(button_rect(box, 1), x, y)) {
        comp_log("client %d force quit%s", c->number, c->pid ? "" : " (pid unknown, connection dropped)");
        if (c->pid > 0)
            kill(c->pid, SIGKILL);
        wire_client_destroy(c->wc);
    }
    return 1;
}
