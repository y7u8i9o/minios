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
 * the only caller. */
static struct {
    struct limine_framebuffer *lfb;
    uint32_t width, height;
    uint32_t fg, bg;                /* packed in the framebuffer layout */
    uint32_t cols, rows;
    uint32_t cx, cy;
    bool present;
    bool disabled;                  /* a user process owns the display */
    char cells[FBCON_MAX_ROWS][FBCON_MAX_COLS];
    /* Escape sequence parser: 0 idle, 1 after ESC, 2 inside CSI. */
    int esc_state;
    unsigned esc_args[4];
    unsigned esc_nargs;
} fb;

static void fbcon_draw_glyph(uint32_t col, uint32_t row, char c, bool inverted)
{
    if (fb.disabled)
        return;
    const uint8_t *glyph = font8x16[(uint8_t)c];
    uint32_t fg = inverted ? fb.bg : fb.fg;
    uint32_t bg = inverted ? fb.fg : fb.bg;
    uint32_t px = col * FONT_WIDTH, py = row * FONT_HEIGHT;
    if (fb.lfb->bpp == 32) {
        volatile uint32_t *dst = (volatile uint32_t *)((volatile uint8_t *)fb.lfb->address +
                                                       py * fb.lfb->pitch) + px;
        uint32_t pitch = (uint32_t)(fb.lfb->pitch / 4);
        for (int y = 0; y < FONT_HEIGHT; y++) {
            uint8_t bits = glyph[y];
            for (int x = 0; x < FONT_WIDTH; x++)
                dst[x] = (bits & (0x80 >> x)) ? fg : bg;
            dst += pitch;
        }
        return;
    }
    for (int y = 0; y < FONT_HEIGHT; y++) {
        uint8_t bits = glyph[y];
        for (int x = 0; x < FONT_WIDTH; x++)
            fb_write_pixel(fb.lfb, px + (uint32_t)x, py + (uint32_t)y, (bits & (0x80 >> x)) ? fg : bg);
    }
}

static void fbcon_clear_screen(void)
{
    for (uint32_t y = 0; y < fb.height; y++)
        for (uint32_t x = 0; x < fb.width; x++)
            fb_write_pixel(fb.lfb, x, y, fb.bg);
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

void fbcon_init(struct limine_framebuffer *lfb)
{
    if (!lfb || (lfb->bpp != 32 && lfb->bpp != 24) || lfb->memory_model != LIMINE_FRAMEBUFFER_RGB) {
        klog_warn("no usable 24 or 32 bpp framebuffer, console is serial only");
        return;
    }
    fb.lfb = lfb;
    fb.width = (uint32_t)lfb->width;
    fb.height = (uint32_t)lfb->height;
    fb.fg = fb_pack_pixel(lfb, FBCON_FG);
    fb.bg = fb_pack_pixel(lfb, FBCON_BG);
    fb.cols = MIN(fb.width / FONT_WIDTH, FBCON_MAX_COLS);
    fb.rows = MIN(fb.height / FONT_HEIGHT, FBCON_MAX_ROWS);
    fb.cx = 0;
    fb.cy = 0;
    memset(fb.cells, ' ', sizeof fb.cells);
    fbcon_clear_screen();
    fb.present = true;
    klog_info("%ux%u framebuffer, %u bpp, %ux%u text cells", fb.width, fb.height, lfb->bpp, fb.cols, fb.rows);
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
