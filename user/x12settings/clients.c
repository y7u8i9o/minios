/* The Surfaces page: the surfaces of X12 in a table. */
#include <stdio.h>
#include <string.h>
#include <gui/model.h>
#include "x12settings.h"
#include "debug-client.h"

struct surf_row { unsigned id, client, mapped, format; char role[16], title[48]; int x, y, w, h; };

static struct widget *table, *surface_count;
static struct surf_row rows[64];
static int nrows, pending_rows;

void clients_surface(uint32_t id, uint32_t client, const char *role, const char *title, int32_t x, int32_t y,
                     int32_t w, int32_t h, uint32_t mapped, uint32_t format)
{
    if (pending_rows >= 64)
        return;
    struct surf_row *r = &rows[pending_rows++];
    r->id = id; r->client = client; r->x = x; r->y = y; r->w = w; r->h = h; r->mapped = mapped; r->format = format;
    strlcpy(r->role, role, sizeof r->role);
    strlcpy(r->title, title, sizeof r->title);
}

void clients_surfaces_done(void)
{
    nrows = pending_rows;
    pending_rows = 0;
    if (table) {
        view_refresh(table);
        char s[48];
        snprintf(s, sizeof s, "%d surface%s", nrows, nrows == 1 ? "" : "s");
        widget_set_text(surface_count, s);
    }
}

static int m_rows(struct model *m, int parent) { return parent < 0 ? nrows : 0; }
static int m_child(struct model *m, int parent, int index) { return index; }
static int m_columns(struct model *m) { return 6; }
static const char *m_cell(struct model *m, int row, int col, char *buf, size_t size)
{
    struct surf_row *r = &rows[row];
    switch (col) {
    case 0: snprintf(buf, size, "%u", r->id); return buf;
    case 1: snprintf(buf, size, "%u", r->client); return buf;
    case 2: return r->role;
    case 3: return r->title;
    case 4: snprintf(buf, size, "%d,%d %dx%d", r->x, r->y, r->w, r->h); return buf;
    default: snprintf(buf, size, "%s%s", r->mapped ? "mapped" : "hidden", r->format == 2 ? " argb" : r->format == 1 ? " xrgb" : ""); return buf;
    }
}
static const char *m_header(struct model *m, int col)
{
    static const char *const names[] = { "Id", "Client", "Role", "Title", "Geometry", "State" };
    return names[col];
}
static struct model model = { m_rows, m_child, m_columns, m_cell, m_header, NULL, NULL, NULL };

void clients_tick(void)
{
    debug_get_surfaces(debug_proxy);
}

static int on_refresh(struct widget *w, void *args, void *arg)
{
    clients_tick();
    gui_flush();
    return 1;
}

void clients_build(struct widget *tabs)
{
    struct widget *surfaces = tabs_add(tabs, "Surfaces");
    struct widget *bar = toolbar_new(surfaces);
    widget_connect(button_new(bar, "Refresh"), "clicked", on_refresh, NULL);
    surface_count = label_new(bar, "");
    table = table_new(surfaces);
    view_set_model(table, &model);
    table_set_column_width(table, 0, 40);
    table_set_column_width(table, 1, 50);
    table_set_column_width(table, 2, 70);
    table_set_column_width(table, 3, 150);
    table_set_column_width(table, 4, 150);
    table_set_column_width(table, 5, 90);
    widget_set_stretch(table, 1, 1);
}
