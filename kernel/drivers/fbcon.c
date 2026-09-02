#define KLOG_SUBSYS "fbcon"
#include <drivers/fbcon.h>
#include <drivers/font.h>
#include <drivers/fbdev.h>
#include <lib/string.h>
#include <klog.h>

#define FBCON_MAX_COLS 256
#define FBCON_MAX_ROWS 128
#define FBCON_FG 0x00aaaaaa
#define FBCON_BG 0x00000000

/* Framebuffer console state. Protected by console_lock in drivers/console.c,
 * the only caller. lfb points at fb_screen. */
static struct {
    const struct limine_framebuffer *lfb;
    uint32_t width, height;
    int32_t dirty_x0, dirty_y0, dirty_x1, dirty_y1;   /* drawn since the last fbcon_take_dirty */
    bool dirty;
    uint32_t fg, bg;                /* packed in the framebuffer layout */
    uint32_t cols, rows;
    uint32_t scale;                 /* pixels per glyph pixel, from video=WxH@N */
    uint32_t cx, cy;
    bool present;
    bool disabled;                  /* a user process owns the display */
    char cells[FBCON_MAX_ROWS][FBCON_MAX_COLS];
    /* Escape sequence parser: 0 idle, 1 after ESC, 2 inside CSI. */
    int esc_state;
    unsigned esc_args[4];
    unsigned esc_nargs;
} fb;

static void mark_dirty(int32_t x0, int32_t y0, int32_t x1, int32_t y1)
{
    if (!fb.dirty) {
        fb.dirty_x0 = x0;
        fb.dirty_y0 = y0;
        fb.dirty_x1 = x1;
        fb.dirty_y1 = y1;
        fb.dirty = true;
        return;
    }
    fb.dirty_x0 = MIN(fb.dirty_x0, x0);
    fb.dirty_y0 = MIN(fb.dirty_y0, y0);
    fb.dirty_x1 = MAX(fb.dirty_x1, x1);
    fb.dirty_y1 = MAX(fb.dirty_y1, y1);
}

bool fbcon_take_dirty(struct fb_rect *r)
{
    if (!fb.dirty)
        return false;
    *r = (struct fb_rect){ fb.dirty_x0, fb.dirty_y0, fb.dirty_x1 - fb.dirty_x0, fb.dirty_y1 - fb.dirty_y0 };
    fb.dirty = false;
    return true;
}

static void fbcon_draw_glyph(uint32_t col, uint32_t row, char c, bool inverted)
{
    if (fb.disabled)
        return;
    const uint8_t *glyph = font8x16[(uint8_t)c];
    uint32_t fg = inverted ? fb.bg : fb.fg;
    uint32_t bg = inverted ? fb.fg : fb.bg;
    uint32_t s = fb.scale;
    uint32_t px = col * FONT_WIDTH * s, py = row * FONT_HEIGHT * s;
    mark_dirty((int32_t)px, (int32_t)py, (int32_t)(px + FONT_WIDTH * s), (int32_t)(py + FONT_HEIGHT * s));
    /* Each glyph pixel covers an s by s block. */
    if (fb.lfb->bpp == 32) {
        volatile uint32_t *dst = (volatile uint32_t *)((volatile uint8_t *)fb.lfb->address +
                                                       py * fb.lfb->pitch) + px;
        uint32_t pitch = (uint32_t)(fb.lfb->pitch / 4);
        for (int y = 0; y < FONT_HEIGHT; y++) {
            uint8_t bits = glyph[y];
            for (uint32_t r = 0; r < s; r++) {
                for (uint32_t x = 0; x < FONT_WIDTH; x++) {
                    uint32_t pix = (bits & (0x80 >> x)) ? fg : bg;
                    for (uint32_t k = 0; k < s; k++)
                        dst[x * s + k] = pix;
                }
                dst += pitch;
            }
        }
        return;
    }
    for (int y = 0; y < FONT_HEIGHT; y++) {
        uint8_t bits = glyph[y];
        for (uint32_t r = 0; r < s; r++)
            for (uint32_t x = 0; x < FONT_WIDTH; x++) {
                uint32_t pix = (bits & (0x80 >> x)) ? fg : bg;
                for (uint32_t k = 0; k < s; k++)
                    fb_write_pixel(fb.lfb, px + x * s + k, py + (uint32_t)y * s + r, pix);
            }
    }
}

static void fbcon_clear_screen(void)
{
    if (fb.lfb->bpp == 32 && fb.lfb->pitch == fb.width * 4) {
        /* Rows are contiguous: fill in one pass. */
        volatile uint32_t *dst = fb.lfb->address;
        for (size_t i = 0; i < (size_t)fb.width * fb.height; i++)
            dst[i] = fb.bg;
    } else {
        for (uint32_t y = 0; y < fb.height; y++)
            for (uint32_t x = 0; x < fb.width; x++)
                fb_write_pixel(fb.lfb, x, y, fb.bg);
    }
    mark_dirty(0, 0, (int32_t)fb.width, (int32_t)fb.height);
}

static void fbcon_draw_row(uint32_t row)
{
    for (uint32_t c = 0; c < fb.cols; c++)
        fbcon_draw_glyph(c, row, fb.cells[row][c], false);
}

static void fbcon_scroll(void)
{
    memmove(fb.cells[0], fb.cells[1], (size_t)(fb.rows - 1) * FBCON_MAX_COLS);
    memset(fb.cells[fb.rows - 1], ' ', FBCON_MAX_COLS);
    for (uint32_t r = 0; r < fb.rows; r++)
        fbcon_draw_row(r);
}

static void fbcon_clear_cells(uint32_t row, uint32_t from, uint32_t to)
{
    for (uint32_t c = from; c < to && c < fb.cols; c++) {
        fb.cells[row][c] = ' ';
        fbcon_draw_glyph(c, row, ' ', false);
    }
}

/* Execute a CSI sequence: ESC [ args cmd. Supported: H (cursor
 * position), J (2: clear screen), K (erase to end of line), A B C D
 * (cursor movement). Everything else is ignored. */
static void fbcon_csi(char cmd)
{
    unsigned a0 = fb.esc_nargs > 0 ? fb.esc_args[0] : 0;
    unsigned a1 = fb.esc_nargs > 1 ? fb.esc_args[1] : 0;
    switch (cmd) {
    case 'H':
    case 'f':
        fb.cy = a0 ? MIN(a0, fb.rows) - 1 : 0;
        fb.cx = a1 ? MIN(a1, fb.cols) - 1 : 0;
        break;
    case 'J':
        if (a0 == 2) {
            for (uint32_t r = 0; r < fb.rows; r++)
                fbcon_clear_cells(r, 0, fb.cols);
            fb.cx = fb.cy = 0;
        } else {
            fbcon_clear_cells(fb.cy, fb.cx, fb.cols);
            for (uint32_t r = fb.cy + 1; r < fb.rows; r++)
                fbcon_clear_cells(r, 0, fb.cols);
        }
        break;
    case 'K':
        fbcon_clear_cells(fb.cy, fb.cx, fb.cols);
        break;
    case 'A': fb.cy -= MIN(a0 ? a0 : 1, fb.cy); break;
    case 'B': fb.cy = MIN(fb.cy + (a0 ? a0 : 1), fb.rows - 1); break;
    case 'C': fb.cx = MIN(fb.cx + (a0 ? a0 : 1), fb.cols - 1); break;
    case 'D': fb.cx -= MIN(a0 ? a0 : 1, fb.cx); break;
    }
}

/* Feed one byte to the escape parser. Returns true if it was consumed. */
static bool fbcon_escape(char c)
{
    if (fb.esc_state == 0) {
        if (c != 27)
            return false;
        fb.esc_state = 1;
        return true;
    }
    if (fb.esc_state == 1) {
        if (c == '[') {
            fb.esc_state = 2;
            fb.esc_nargs = 0;
            fb.esc_args[0] = 0;
        } else {
            fb.esc_state = 0;
        }
        return true;
    }
    if (c >= '0' && c <= '9') {
        if (fb.esc_nargs == 0)
            fb.esc_nargs = 1;
        fb.esc_args[fb.esc_nargs - 1] = fb.esc_args[fb.esc_nargs - 1] * 10 + (unsigned)(c - '0');
        return true;
    }
    if (c == ';') {
        if (fb.esc_nargs < ARRAY_SIZE(fb.esc_args)) {
            if (fb.esc_nargs == 0)
                fb.esc_nargs = 1;
            fb.esc_args[fb.esc_nargs++] = 0;
        }
        return true;
    }
    if (c == '?')
        return true;    /* private mode prefix, sequence ignored below */
    fbcon_csi(c);
    fb.esc_state = 0;
    return true;
}

static void fbcon_put_raw(char c)
{
    if (fbcon_escape(c))
        return;
    switch (c) {
    case '\n':
        fb.cx = 0;
        fb.cy++;
        break;
    case '\r':
        fb.cx = 0;
        break;
    case '\t':
        fb.cx = (fb.cx + 8) & ~7u;
        break;
    case '\b':
        if (fb.cx > 0)
            fb.cx--;
        break;
    default:
        fb.cells[fb.cy][fb.cx] = c;
        fbcon_draw_glyph(fb.cx, fb.cy, c, false);
        fb.cx++;
        break;
    }
    if (fb.cx >= fb.cols) {
        fb.cx = 0;
        fb.cy++;
    }
    if (fb.cy >= fb.rows) {
        fbcon_scroll();
        fb.cy = fb.rows - 1;
    }
}

static bool screen_usable(void)
{
    return fb_screen_present && (fb_screen.bpp == 32 || fb_screen.bpp == 24) &&
           fb_screen.memory_model == LIMINE_FRAMEBUFFER_RGB;
}

/* Take the geometry and the pixel layout from fb_screen. */
static void fbcon_setup(void)
{
    fb.lfb = &fb_screen;
    fb.width = (uint32_t)fb_screen.width;
    fb.height = (uint32_t)fb_screen.height;
    fb.fg = fb_pack_pixel(fb.lfb, FBCON_FG);
    fb.bg = fb_pack_pixel(fb.lfb, FBCON_BG);
    fb.scale = fb_screen_scale ? fb_screen_scale : 1;
    /* A scale the mode cannot hold (fewer than 40x12 cells) is ignored. */
    if (fb.width / (FONT_WIDTH * fb.scale) < 40 || fb.height / (FONT_HEIGHT * fb.scale) < 12)
        fb.scale = 1;
    fb.cols = MIN(fb.width / (FONT_WIDTH * fb.scale), FBCON_MAX_COLS);
    fb.rows = MIN(fb.height / (FONT_HEIGHT * fb.scale), FBCON_MAX_ROWS);
}

void fbcon_init(void)
{
    if (!screen_usable()) {
        klog_warn("no usable 24 or 32 bpp framebuffer, console is serial only");
        return;
    }
    fbcon_setup();
    fb.cx = 0;
    fb.cy = 0;
    memset(fb.cells, ' ', sizeof fb.cells);
    fbcon_clear_screen();
    fb.present = true;
    klog_info("%ux%u framebuffer, %u bpp, %ux%u text cells, scale %u", fb.width, fb.height, fb.lfb->bpp,
              fb.cols, fb.rows, fb.scale);
}

void fbcon_screen_changed(void)
{
    if (!screen_usable()) {
        fb.present = false;
        return;
    }
    bool was_present = fb.present;
    fbcon_setup();
    if (!was_present) {
        fb.cx = 0;
        fb.cy = 0;
        memset(fb.cells, ' ', sizeof fb.cells);
    }
    if (fb.cx >= fb.cols)
        fb.cx = fb.cols - 1;
    if (fb.cy >= fb.rows) {
        /* Keep the last rows of text when the grid shrinks. */
        uint32_t drop = fb.cy - (fb.rows - 1);
        memmove(fb.cells[0], fb.cells[drop], (size_t)(FBCON_MAX_ROWS - drop) * FBCON_MAX_COLS);
        memset(fb.cells[FBCON_MAX_ROWS - drop], ' ', (size_t)drop * FBCON_MAX_COLS);
        fb.cy = fb.rows - 1;
    }
    fb.present = true;
    if (fb.disabled)
        return;
    fbcon_clear_screen();
    for (uint32_t r = 0; r < fb.rows; r++)
        fbcon_draw_row(r);
    fbcon_draw_glyph(fb.cx, fb.cy, fb.cells[fb.cy][fb.cx], true);
}

bool fbcon_present(void)
{
    return fb.present;
}

void fbcon_write(const char *s, size_t n)
{
    if (!fb.present)
        return;
    /* Erase the cursor, write, redraw the cursor at the new position. */
    fbcon_draw_glyph(fb.cx, fb.cy, fb.cells[fb.cy][fb.cx], false);
    for (size_t i = 0; i < n; i++)
        fbcon_put_raw(s[i]);
    fbcon_draw_glyph(fb.cx, fb.cy, fb.cells[fb.cy][fb.cx], true);
}

void fbcon_putc(char c)
{
    fbcon_write(&c, 1);
}

void fbcon_get_size(uint16_t *cols, uint16_t *rows)
{
    *cols = fb.present ? (uint16_t)fb.cols : 80;
    *rows = fb.present ? (uint16_t)fb.rows : 25;
}

void fbcon_set_enabled(bool enabled)
{
    if (!fb.present)
        return;
    /* Called through console_lock by the fb0 device, which takes it. */
    fb.disabled = !enabled;
    if (enabled) {
        fbcon_clear_screen();
        for (uint32_t r = 0; r < fb.rows; r++)
            fbcon_draw_row(r);
        fbcon_draw_glyph(fb.cx, fb.cy, fb.cells[fb.cy][fb.cx], true);
    }
}
