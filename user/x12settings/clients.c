/* The Clients page: the clients of X12 with their programs and buffer
 * memory, a tree of their surfaces, the details and a thumbnail of the
 * selected surface, and its outline on the screen. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <gui/client.h>
#include <gui/model.h>
#include <gui/pixel.h>
#include <minios/proctab.h>
#include "x12settings.h"
#include "core-client.h"
#include "debug-client.h"

#define MAX_CLIENTS 32
#define MAX_SURFACES 128
#define CLIENT_ROW 100000               /* tree rows of clients start here, surfaces use their index */
#define THUMB_W 240
#define THUMB_H 160

struct client_row {
    unsigned number, pid, uid, surfaces, pool_bytes, not_responding;
    char program[32];
};

struct surface_row {
    unsigned id, client, mapped, format;
    char role[16], title[48];
    int x, y, w, h;
};

static struct client_row clients[MAX_CLIENTS], pending_clients[MAX_CLIENTS];
static int nclients, npending_clients;
static struct surface_row surfaces[MAX_SURFACES], pending_surfaces[MAX_SURFACES];
static int nsurfaces, npending_surfaces;

static struct widget *tree, *details, *thumb;
static unsigned selected_id;            /* the selected surface, 0 for none */

/* The thumbnail: a shared buffer that X12 fills with capture_surface. */
static struct wire_proxy *shm, *thumb_buffer;
static uint32_t *thumb_pixels;
static int thumb_w, thumb_h;            /* the part that the last capture used */
static int capture_done, capture_ok;

/* ---- the data ---- */

void clients_client(uint32_t number, uint32_t pid, uint32_t uid, uint32_t nsurf, uint32_t pool_bytes,
                    uint32_t not_responding)
{
    if (npending_clients == MAX_CLIENTS)
        return;
    struct client_row *c = &pending_clients[npending_clients++];
    *c = (struct client_row){ number, pid, uid, nsurf, pool_bytes, not_responding, "" };
    struct proc_entry e;
    if (pid && proc_table_find((pid_t)pid, &e) == 0)
        snprintf(c->program, sizeof c->program, "%s", e.name);
}

void clients_done(void)
{
    memcpy(clients, pending_clients, sizeof clients);
    nclients = npending_clients;
    npending_clients = 0;
}

void clients_surface(uint32_t id, uint32_t client, const char *role, const char *title, int32_t x, int32_t y,
                     int32_t w, int32_t h, uint32_t mapped, uint32_t format)
{
    if (npending_surfaces == MAX_SURFACES)
        return;
    struct surface_row *r = &pending_surfaces[npending_surfaces++];
    *r = (struct surface_row){ id, client, mapped, format, "", "", x, y, w, h };
    snprintf(r->role, sizeof r->role, "%s", role);
    snprintf(r->title, sizeof r->title, "%s", title);
}

void clients_surfaces_done(void)
{
    memcpy(surfaces, pending_surfaces, sizeof surfaces);
    nsurfaces = npending_surfaces;
    npending_surfaces = 0;
    if (tree) {
        /* Every client shows its surfaces. */
        for (int i = 0; i < nclients; i++)
            if (!treeview_is_expanded(tree, CLIENT_ROW + i))
                treeview_expand(tree, CLIENT_ROW + i, 1);
        view_refresh(tree);
    }
}

static const char *format_name(unsigned format)
{
    return format == 2 ? "ARGB8888" : format == 1 ? "XRGB8888" : "no buffer";
}

static void format_kib(unsigned bytes, char *buf, size_t size)
{
    if (bytes >= 10u << 20)
        snprintf(buf, size, "%u MiB", bytes >> 20);
    else
        snprintf(buf, size, "%u KiB", bytes >> 10);
}

/* ---- the tree ---- */

/* The index-th surface of the client with the number. */
static int surface_of(unsigned number, int index)
{
    for (int i = 0; i < nsurfaces; i++)
        if (surfaces[i].client == number && index-- == 0)
            return i;
    return -1;
}

static int m_rows(struct model *m, int parent)
{
    if (parent < 0)
        return nclients;
    if (parent < CLIENT_ROW)
        return 0;
    int n = 0;
    while (surface_of(clients[parent - CLIENT_ROW].number, n) >= 0)
        n++;
    return n;
}

static int m_child(struct model *m, int parent, int index)
{
    return parent < 0 ? CLIENT_ROW + index : surface_of(clients[parent - CLIENT_ROW].number, index);
}

static int m_columns(struct model *m) { return 1; }

static const char *m_cell(struct model *m, int row, int col, char *buf, size_t size)
{
    /* The tree view shows the first column, so it contains everything. */
    if (row >= CLIENT_ROW) {
        struct client_row *c = &clients[row - CLIENT_ROW];
        char kib[24];
        format_kib(c->pool_bytes, kib, sizeof kib);
        snprintf(buf, size, "Client %u: %s, pid %u, uid %u, %s of buffers%s", c->number,
                 c->program[0] ? c->program : "unknown program", c->pid, c->uid, kib,
                 c->not_responding ? ", not responding" : "");
        return buf;
    }
    struct surface_row *s = &surfaces[row];
    snprintf(buf, size, "Surface %u: %s%s%s%s, %dx%d at %d,%d%s", s->id, s->role, s->title[0] ? " \"" : "",
             s->title, s->title[0] ? "\"" : "", s->w, s->h, s->x, s->y, s->mapped ? "" : ", not mapped");
    return buf;
}

static const char *m_header(struct model *m, int col)
{
    return "Clients and their surfaces";
}

static struct model model = { m_rows, m_child, m_columns, m_cell, m_header, NULL, NULL, NULL };

/* ---- the thumbnail and the details ---- */

static int make_thumb_buffer(void)
{
    if (thumb_buffer)
        return 0;
    shm = gui_bind_global("shm", &shm_interface, 1);
    if (!shm)
        return -1;
    size_t bytes = (size_t)THUMB_W * THUMB_H * 4;
    int fd = memfd_create("x12settings", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, (long)bytes) < 0)
        return -1;
    thumb_pixels = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (thumb_pixels == MAP_FAILED) {
        close(fd);
        thumb_pixels = NULL;
        return -1;
    }
    struct wire_proxy *pool = shm_create_pool(shm, fd, (int32_t)bytes);
    thumb_buffer = shm_pool_create_buffer(pool, 0, THUMB_W, THUMB_H, THUMB_W * 4, 2);
    close(fd);
    return 0;
}

void clients_captured(uint32_t id, int32_t w, int32_t h)
{
    thumb_w = w;
    thumb_h = h;
    capture_done = capture_ok = 1;
    if (thumb)
        widget_invalidate(thumb);
}

void clients_capture_failed(uint32_t id)
{
    thumb_w = thumb_h = 0;
    capture_done = 1;
    capture_ok = 0;
    if (thumb)
        widget_invalidate(thumb);
}

void clients_surface_info(uint32_t id, uint32_t found, int32_t bw, int32_t bh, int32_t scale, int32_t transform,
                          uint32_t format, uint32_t opaque, uint32_t commits)
{
    if (!details || id != selected_id)
        return;
    char text[256];
    if (!found)
        snprintf(text, sizeof text, "Surface %u no longer exists.", id);
    else
        snprintf(text, sizeof text,
                 "Surface %u: buffer %dx%d, scale %d, transform %d, %s, %u opaque rectangles, %u commits", id, bw,
                 bh, scale, transform, format_name(format), opaque, commits);
    widget_set_text(details, text);
}

static int on_thumb_paint(struct widget *w, void *args, void *arg)
{
    struct painter *p = ((struct sig_paint *)args)->p;
    const struct theme *t = p->theme;
    painter_fill(p, 0, 0, w->w, w->h, t->color[TC_FIELD]);
    if (thumb_pixels && thumb_w > 0 && thumb_h > 0) {
        /* The capture keeps the alpha of the buffer: the image is blended
         * over squares of 8 pixels, which show the transparent parts. */
        static uint32_t blended[THUMB_W * THUMB_H];
        for (int y = 0; y < thumb_h; y++)
            for (int x = 0; x < thumb_w; x++) {
                uint32_t c = thumb_pixels[y * THUMB_W + x], bg = (x / 8 + y / 8) % 2 ? 0x00d0d0d0 : t->color[TC_FIELD];
                blended[y * thumb_w + x] = pixel_blend(bg, c & 0xffffff, c >> 24);
            }
        struct surface s = { blended, thumb_w, thumb_h, thumb_w };
        painter_blit(p, 0, 0, &s);
    }
    painter_frame(p, 0, 0, w->w, w->h, t->color[TC_BORDER]);
    return 1;
}

static void select_surface(unsigned id)
{
    selected_id = id;
    debug_highlight(debug_proxy, id);
    if (!id) {
        widget_set_text(details, "Select a surface to see its details and its outline on the screen.");
        thumb_w = thumb_h = 0;
        widget_invalidate(thumb);
        return;
    }
    debug_get_surface(debug_proxy, id);
    if (make_thumb_buffer() == 0)
        debug_capture_surface(debug_proxy, id, thumb_buffer);
}

static int on_select(struct widget *w, void *args, void *arg)
{
    int row = ((struct sig_select *)args)->index;
    select_surface(row >= 0 && row < CLIENT_ROW && row < nsurfaces ? surfaces[row].id : 0);
    gui_flush();
    return 1;
}

void clients_tick(void)
{
    debug_get_clients(debug_proxy);
    debug_get_surfaces(debug_proxy);
    if (selected_id) {
        debug_get_surface(debug_proxy, selected_id);
        if (thumb_buffer)
            debug_capture_surface(debug_proxy, selected_id, thumb_buffer);
    }
}

void clients_build(struct widget *tabs)
{
    struct widget *page = tabs_add(tabs, "Clients");
    struct widget *split = splitpane_new(page, 0);
    widget_set_stretch(split, 1, 1);
    tree = treeview_new(split);
    view_set_model(tree, &model);
    widget_connect(tree, "selected", on_select, NULL);
    struct widget *side = box_new(split, 1);
    thumb = canvas_new(side);
    widget_set_hint(thumb, THUMB_W, THUMB_H);
    widget_connect(thumb, "paint", on_thumb_paint, NULL);
    splitpane_set_position(split, 470);
    details = label_new(page, "Select a surface to see its details and its outline on the screen.");
}

/* ---- the command line ---- */

static unsigned id_of_title(const char *title)
{
    for (int i = 0; i < nsurfaces; i++)
        if (strcmp(surfaces[i].title, title) == 0)
            return surfaces[i].id;
    return 0;
}

int clients_command(int argc, char **argv)
{
    struct wire_display *d = gui_display();
    debug_get_clients(debug_proxy);
    debug_get_surfaces(debug_proxy);
    wire_display_roundtrip(d);
    if (strcmp(argv[1], "clients") == 0) {
        for (int i = 0; i < nclients; i++) {
            struct client_row *c = &clients[i];
            printf("x12settings: client %u pid %u program %s surfaces %u pools %u bytes\n", c->number, c->pid,
                   c->program[0] ? c->program : "-", c->surfaces, c->pool_bytes);
        }
        for (int i = 0; i < nsurfaces; i++)
            printf("x12settings: surface %u client %u role %s title \"%s\"\n", surfaces[i].id, surfaces[i].client,
                   surfaces[i].role, surfaces[i].title);
        fflush(stdout);
        return 0;
    }
    unsigned id = argc >= 3 ? id_of_title(argv[2]) : 0;
    if (!id) {
        fprintf(stderr, "x12settings: no surface with this title\n");
        return 1;
    }
    if (strcmp(argv[1], "capture") == 0) {
        if (make_thumb_buffer() < 0)
            return 1;
        debug_capture_surface(debug_proxy, id, thumb_buffer);
        wire_display_roundtrip(d);
        if (!capture_ok) {
            printf("x12settings: capture of surface %u failed\n", id);
            return 1;
        }
        printf("x12settings: captured surface %u at %dx%d, centre 0x%06x\n", id, thumb_w, thumb_h,
               thumb_pixels[(thumb_h / 2) * THUMB_W + thumb_w / 2] & 0xffffff);
        fflush(stdout);
        return 0;
    }
    if (strcmp(argv[1], "highlight") == 0) {
        debug_highlight(debug_proxy, id);
        wire_display_roundtrip(d);
        printf("x12settings: surface %u outlined\n", id);
        fflush(stdout);
        sleep(argc >= 4 ? (unsigned)atoi(argv[3]) : 2);
        return 0;
    }
    return 2;
}
