/* The audio applet of the panel: a speaker button left of the clock
 * opens a popup listing the master volume and every stream of the
 * audio server with a volume bar and a level meter.  The popup contains
 * an audio connection while it is open; the bars are dragged with the
 * pointer. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ipc.h>
#include <audio/audio.h>
#include <gui/i18n.h>
#include "panel.h"

#define MIXER_W 280
#define ROW_H 44
#define MIXER_PAD 8
#define NAME_W 96
#define BAR_H 8
#define METER_H 3
#define MAX_ROWS 8

static struct canvas mixer;
static struct wire_proxy *popup;
static struct audio_connection *audio;
static struct audio_mixer *view;
static int mixer_open;
static int drag_row = -1;           /* 0 is the master row, then the streams */
static int rows;                    /* rows drawn, the master row included */
static uint32_t drawn_generation;

int mixer_button_x(void)
{
    return clock_x() - MIXER_BTN_W - 4;
}

/* A small speaker glyph: a box, a cone and two arcs. */
void mixer_draw_button(struct painter *p, int hovered)
{
    int x = mixer_button_x(), h = panel.lh;
    if (mixer_open || hovered)
        painter_rounded(p, x, BUTTON_Y, MIXER_BTN_W, h - 2 * BUTTON_Y, mixer_open ? BUTTON_OPEN : BUTTON_HOVER,
                        0xffffffffu);
    int cx = x + MIXER_BTN_W / 2 - 3, cy = h / 2;
    painter_fill(p, cx - 5, cy - 2, 3, 5, PANEL_TEXT);
    for (int i = 0; i < 5; i++)
        painter_fill(p, cx - 2 + i, cy - 2 - i, 1, 5 + 2 * i, PANEL_TEXT);
    painter_line(p, cx + 5, cy - 3, cx + 6, cy - 1, PANEL_TEXT_DIM);
    painter_line(p, cx + 6, cy - 1, cx + 6, cy + 1, PANEL_TEXT_DIM);
    painter_line(p, cx + 6, cy + 1, cx + 5, cy + 3, PANEL_TEXT_DIM);
    painter_line(p, cx + 8, cy - 5, cx + 9, cy - 2, PANEL_TEXT_DIM);
    painter_line(p, cx + 9, cy - 2, cx + 9, cy + 2, PANEL_TEXT_DIM);
    painter_line(p, cx + 9, cy + 2, cx + 8, cy + 5, PANEL_TEXT_DIM);
}

int mixer_is_open(void)
{
    return mixer_open;
}

int mixer_owns(const struct wire_proxy *surface)
{
    return mixer.surface && surface == mixer.surface;
}

static int popup_height(void)
{
    int n = 1 + (view ? audio_mixer_count(view) : 0);
    if (n > MAX_ROWS)
        n = MAX_ROWS;
    return n * ROW_H + 2 * MIXER_PAD;
}

static int bar_x(void)
{
    return MIXER_PAD + NAME_W;
}

static int bar_w(void)
{
    return MIXER_W - bar_x() - MIXER_PAD;
}

static void draw_row(struct painter *p, int row, const char *name, const char *detail,
                     unsigned volume, unsigned peak, int running)
{
    int y = MIXER_PAD + row * ROW_H;
    painter_push(p, MIXER_PAD, y, NAME_W - 6, ROW_H);
    int th = painter_text_height(p);
    painter_text(p, 0, 6, name, running ? MENU_TEXT : MENU_TEXT_DIM);
    painter_text(p, 0, 6 + th + 2, detail, MENU_TEXT_DIM);
    painter_pop(p);
    int bx = bar_x(), bw = bar_w();
    int by = y + 12;
    int filled = (int)((unsigned)bw * (volume > 100 ? 100 : volume) / 100);
    painter_rounded(p, bx, by, bw, BAR_H, METER_BG, 0xffffffffu);
    if (filled > 0)
        painter_rounded(p, bx, by, filled, BAR_H, ACCENT, 0xffffffffu);
    int kx = bx + filled - 5;
    if (kx < bx) kx = bx;
    if (kx > bx + bw - 10) kx = bx + bw - 10;
    painter_rounded(p, kx, by - 3, 10, BAR_H + 6, MENU_BG, MENU_BORDER);
    int my = by + BAR_H + 8;
    painter_fill(p, bx, my, bw, METER_H, METER_BG);
    int level = (int)((unsigned)bw * (peak > 32767 ? 32767 : peak) / 32767);
    if (level > 0)
        painter_fill(p, bx, my, level, METER_H, METER_FG);
}

static void draw_mixer(void)
{
    if (!mixer.surface)
        return;
    struct painter p;
    canvas_painter(&p, &mixer);
    int w = mixer.lw, h = mixer.lh;
    painter_fill(&p, 0, 0, w, h, MENU_BG);
    painter_frame(&p, 0, 0, w, h, MENU_BORDER);
    if (!view) {
        panel_label(&p, 0, 0, w, h, _("No audio service"), MENU_TEXT_DIM, 1);
        canvas_commit(&mixer);
        rows = 0;
        return;
    }
    char detail[32];
    snprintf(detail, sizeof detail, "%u%%", audio_mixer_master(view));
    draw_row(&p, 0, _("Output"), detail, audio_mixer_master(view), 0, 1);
    rows = 1;
    /* The popup retains the size it opened with: rows beyond it are counted. */
    int fit = (h - 2 * MIXER_PAD) / ROW_H;
    int n = audio_mixer_count(view);
    for (int i = 0; i < n && rows < fit; i++) {
        const struct audio_mixer_stream *s = audio_mixer_stream(view, i);
        const char *format = s->direction                         ? _("capture %u%%")
                             : s->state == AUDIO_PLAYBACK_RUNNING ? _("playing %u%%")
                                                                  : _("paused %u%%");
        snprintf(detail, sizeof detail, format, s->volume);
        draw_row(&p, rows, s->name, detail, s->volume, s->peak, s->state == AUDIO_PLAYBACK_RUNNING);
        rows++;
    }
    if (n == 0)
        panel_label(&p, 0, MIXER_PAD + ROW_H, w, ROW_H, _("No streams"), MENU_TEXT_DIM, 1);
    else if (n > rows - 1) {
        snprintf(detail, sizeof detail, ngettext("%d more", "%d more", n - (rows - 1)), n - (rows - 1));
        panel_label(&p, 0, h - MIXER_PAD - 14, w, 14, detail, MENU_TEXT_DIM, 1);
    }
    drawn_generation = audio_mixer_generation(view);
    canvas_commit(&mixer);
}

/* ---- the popup ---- */

static void mixer_teardown(void)
{
    if (popup) {
        popup_destroy(popup);
        popup = NULL;
    }
    if (mixer.surface) {
        surface_attach(mixer.surface, NULL, 0, 0);
        surface_commit(mixer.surface);
        surface_destroy(mixer.surface);
        canvas_release_buffer(&mixer);
        memset(&mixer, 0, sizeof mixer);
    }
    if (view) {
        audio_mixer_destroy(view);
        view = NULL;
    }
    if (audio) {
        audio_disconnect(audio);
        audio = NULL;
    }
    mixer_open = 0;
    drag_row = -1;
}

static void on_popup_configure(void *user, struct wire_proxy *p, uint32_t serial,
                               int32_t x, int32_t y, int32_t w, int32_t h)
{
    popup_ack_configure(p, serial);
    popup_grab(popup, seat, press_serial);
    mixer_open = 1;
    draw_mixer();
    log_line("mixer opened, %d streams", view ? audio_mixer_count(view) : -1);
    draw_panel();
}

static void on_popup_done(void *user, struct wire_proxy *p)
{
    mixer_teardown();
    log_line("mixer closed");
    draw_panel();
}

static const struct popup_listener popup_events = { on_popup_configure, on_popup_done };

static void mixer_show(void)
{
    audio = audio_connect();
    view = audio ? audio_mixer_create(audio) : NULL;
    int h = popup_height();
    if (canvas_create(&mixer, MIXER_W, h) < 0) {
        mixer_teardown();
        return;
    }
    struct wire_proxy *pos = shell_create_positioner(shell);
    positioner_set_size(pos, MIXER_W, h);
    panel_place_popup(pos, mixer_button_x(), MIXER_BTN_W, 1);
    popup = shell_get_popup(shell, mixer.surface, panel.surface, pos);
    popup_add_listener(popup, &popup_events, NULL);
    positioner_destroy(pos);
    mixer_open = 1;                 /* opening; configure finishes it */
    wire_display_flush(display);
    draw_panel();
}

void mixer_toggle(void)
{
    if (mixer_open)
        mixer_teardown();
    else
        mixer_show();
    draw_panel();
}

/* ---- pointer ---- */

static int row_at(int y)
{
    if (y < MIXER_PAD)
        return -1;
    int row = (y - MIXER_PAD) / ROW_H;
    return row < rows ? row : -1;
}

static void set_row_volume(int row, int x)
{
    if (!view)
        return;
    int bw = bar_w();
    int v = (x - bar_x()) * 100 / (bw > 0 ? bw : 1);
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    if (row == 0) {
        audio_mixer_set_master(view, (unsigned)v);
        log_line("mixer master %d", v);
    } else {
        const struct audio_mixer_stream *s = audio_mixer_stream(view, row - 1);
        if (!s)
            return;
        audio_mixer_set_volume(view, s->id, (unsigned)v);
        log_line("mixer stream %s %d", s->name, v);
    }
}

void mixer_pointer_motion(int x, int y)
{
    if (drag_row >= 0)
        set_row_volume(drag_row, x);
}

void mixer_pointer_button(uint32_t button, uint32_t state, int x, int y)
{
    if (button != 1)
        return;
    if (state == 0) {
        drag_row = -1;
        return;
    }
    int row = row_at(y);
    if (row < 0)
        return;
    drag_row = row;
    set_row_volume(row, x);
}

/* ---- the audio connection ---- */

int mixer_fd(void)
{
    return mixer_open && audio ? audio_connection_fd(audio) : -1;
}

void mixer_dispatch(int revents)
{
    if (!audio)
        return;
    if (revents & (POLLERR | POLLHUP | POLLNVAL) || audio_connection_dispatch(audio, 0) < 0) {
        audio_mixer_destroy(view);
        view = NULL;
        audio_disconnect(audio);
        audio = NULL;
        if (mixer.surface)
            draw_mixer();
        return;
    }
    if (view && audio_mixer_generation(view) != drawn_generation)
        draw_mixer();
}
