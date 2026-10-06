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

/* Terminal window palette; index 7 retains the console's historical grey. */
static const uint32_t palette[16] = {
    0x000000, 0xcd3131, 0x0dbc79, 0xe5e510, 0x2472c8, 0xbc3fbc, 0x11a8cd, 0xaaaaaa,
    0x666666, 0xf14c4c, 0x23d18b, 0xf5f543, 0x3b8eea, 0xd670d6, 0x29b8db, 0xffffff,
};

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
    uint8_t attrs[FBCON_MAX_ROWS][FBCON_MAX_COLS];
    uint8_t fg_index, bg_index;
    bool bold, reverse;
    /* Escape sequence parser: 0 idle, 1 after ESC, 2 inside CSI. */
    int esc_state;
    unsigned esc_args[8];
    unsigned esc_nargs;
} fb;

static uint8_t current_attr(void)
{
    uint8_t fg = fb.fg_index | (fb.bold ? 8 : 0), bg = fb.bg_index;
    return fb.reverse ? (uint8_t)(fg << 4 | bg) : (uint8_t)(bg << 4 | fg);
}

bool fbcon_get_cell(uint32_t col, uint32_t row, char *c, uint8_t *attr)
{
    if (!fb.present || col >= fb.cols || row >= fb.rows) return false;
    if (c) *c = fb.cells[row][col];
    if (attr) *attr = fb.attrs[row][col];
    return true;
}

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

/* While a user process owns the display, the buffer contains its pixels,
 * which only that process flushes. */
bool fbcon_take_dirty(struct fb_rect *r)
{
    if (!fb.dirty || fb.disabled)
        return false;
    *r = (struct fb_rect){ fb.dirty_x0, fb.dirty_y0, fb.dirty_x1 - fb.dirty_x0, fb.dirty_y1 - fb.dirty_y0 };
    fb.dirty = false;
    return true;
}

static void fbcon_draw_glyph(uint32_t col, uint32_t row, char c, bool inverted)
{
    if (fb.disabled || col >= fb.cols || row >= fb.rows)
        return;
    const uint8_t *glyph = font8x16[(uint8_t)c];
    uint8_t attr = fb.attrs[row][col];
    uint32_t fg = fb_pack_pixel(fb.lfb, palette[inverted ? attr >> 4 : attr & 15]);
    uint32_t bg = fb_pack_pixel(fb.lfb, palette[inverted ? attr & 15 : attr >> 4]);
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
    memmove(fb.attrs[0], fb.attrs[1], (size_t)(fb.rows - 1) * FBCON_MAX_COLS);
    memset(fb.attrs[fb.rows - 1], current_attr(), FBCON_MAX_COLS);
    for (uint32_t r = 0; r < fb.rows; r++)
        fbcon_draw_row(r);
}

static void fbcon_clear_cells(uint32_t row, uint32_t from, uint32_t to)
{
    for (uint32_t c = from; c < to && c < fb.cols; c++) {
        fb.cells[row][c] = ' ';
        fb.attrs[row][c] = current_attr();
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
    case 'm':
        for (unsigned i = 0; i < (fb.esc_nargs ? fb.esc_nargs : 1); i++) {
            unsigned a = fb.esc_args[i];
            if (a == 0) { fb.fg_index = 7; fb.bg_index = 0; fb.bold = fb.reverse = false; }
            else if (a == 1) fb.bold = true;
            else if (a == 22) fb.bold = false;
            else if (a == 7) fb.reverse = true;
            else if (a == 27) fb.reverse = false;
            else if (a >= 30 && a <= 37) fb.fg_index = a - 30;
            else if (a == 39) fb.fg_index = 7;
            else if (a >= 40 && a <= 47) fb.bg_index = a - 40;
            else if (a == 49) fb.bg_index = 0;
            else if (a >= 90 && a <= 97) fb.fg_index = a - 90 + 8;
            else if (a >= 100 && a <= 107) fb.bg_index = a - 100 + 8;
        }
        break;
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

/* The UTF-8 decoder of the console output: the code point being decoded
 * and the number of continuation bytes still expected.  Protected by the
 * caller's console_lock, as the rest of fb. */
static uint32_t utf8_cp;
static int utf8_need;

static void fbcon_put_raw(char c)
{
    if (fbcon_escape(c))
        return;
    /* Output is UTF-8.  The font has glyphs for the code points below 256,
     * and other characters are drawn as a question mark. */
    unsigned char u = (unsigned char)c;
    if (utf8_need && (u & 0xc0) == 0x80) {
        utf8_cp = utf8_cp << 6 | (u & 0x3f);
        if (--utf8_need)
            return;
        c = utf8_cp < 256 ? (char)utf8_cp : '?';
    } else if (u >= 0xc0 && u < 0xf8) {
        utf8_need = u >= 0xf0 ? 3 : u >= 0xe0 ? 2 : 1;
        utf8_cp = u & (0x3f >> utf8_need);
        return;
    } else {
        utf8_need = 0;
        if (u >= 0x80)
            c = '?';
    }
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
        fb.attrs[fb.cy][fb.cx] = current_attr();
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
    /* A scale the mode cannot contain (fewer than 40x12 cells) is ignored. */
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
    fb.fg_index = 7;
    fb.bg_index = 0;
    fb.bold = fb.reverse = false;
    memset(fb.attrs, 7, sizeof fb.attrs);
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
        memset(fb.attrs, 7, sizeof fb.attrs);
    }
    if (fb.cx >= fb.cols)
        fb.cx = fb.cols - 1;
    if (fb.cy >= fb.rows) {
        /* Retain the last rows of text when the grid shrinks. */
        uint32_t drop = fb.cy - (fb.rows - 1);
        memmove(fb.cells[0], fb.cells[drop], (size_t)(FBCON_MAX_ROWS - drop) * FBCON_MAX_COLS);
        memmove(fb.attrs[0], fb.attrs[drop], (size_t)(FBCON_MAX_ROWS - drop) * FBCON_MAX_COLS);
        memset(fb.attrs[FBCON_MAX_ROWS - drop], 7, (size_t)drop * FBCON_MAX_COLS);
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
    /* The panic path prints from a CPU that halted the console daemon,
     * possibly between advancing the cursor past the last row and the
     * scroll that follows. Finish that scroll before drawing anything. */
    if (fb.cy >= fb.rows) {
        fbcon_scroll();
        fb.cy = fb.rows - 1;
    }
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
    /* Called through console_lock by the fb0 device, which takes it. A
     * region that the console drew before the owner acquired the display
     * is not flushed any more: the flush would show pixels of the owner
     * that the owner has not flushed yet. */
    fb.disabled = !enabled;
    if (!enabled)
        fb.dirty = false;
    if (enabled) {
        fbcon_clear_screen();
        for (uint32_t r = 0; r < fb.rows; r++)
            fbcon_draw_row(r);
        fbcon_draw_glyph(fb.cx, fb.cy, fb.cells[fb.cy][fb.cx], true);
    }
}
