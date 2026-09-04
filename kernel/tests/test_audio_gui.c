/* M34: the desktop audio applications: the WAV player, the step
 * sequencer and the mixer applet of the panel, each against a running
 * audio server. */
#include <tests/ktest.h>
#include <drivers/fbdev.h>
#include <drivers/timer.h>
#include <sched/user.h>
#include <sched/proc.h>
#include <ipc/signal.h>
#include <console.h>
#include <fs/vfs.h>
#include "gui_helpers.h"

#define PANEL_H 28
#define CLOCK_W 80
#define MIXER_BTN_W 30
#define MIXER_W 280
#define MIXER_ROW_H 44
#define MIXER_PAD 8
#define PANEL_ACCENT 0x005b9cf5     /* the panel's own palette */
#define METER_BG 0x00e3e6ea
#define ACCENT 0x003c78c8           /* TC_ACCENT of the libgui theme */

static struct proc *start_audiod(void)
{
    struct proc *p = proc_create_user("/bin/audiod", (char *const[]){ "audiod", NULL },
                                      (char *const[]){ NULL }, &kernel_proc);
    ktest_assert(p != NULL, "cannot start audiod");
    sleep_ms(300);
    return p;
}

static void stop_audiod(struct proc *p)
{
    signal_send(p, SIGTERM);
    int status = proc_reap(p);
    ktest_assert(status == 0, "audiod status 0x%x", status);
}

/* Pixels of the given colour in a logical rectangle. */
static int count_color(int x0, int y0, int w, int h, uint32_t color)
{
    int n = 0;
    for (int y = y0; y < y0 + h; y++)
        for (int x = x0; x < x0 + w; x++)
            if (pixel(x, y) == color)
                n++;
    return n;
}

/* The player draws the chime's waveform and reports the end of the file;
 * space restarts it. */
static void test_audio_player(void)
{
    ktest_assert(fb_screen_present, "no framebuffer");
    struct proc *audiod = start_audiod();
    struct proc *srv = start_server();
    struct proc *cl = proc_create_user("/bin/player",
                                       (char *const[]){ "player", "/usr/share/sounds/chime.wav", NULL },
                                       (char *const[]){ NULL }, &kernel_proc);
    ktest_assert(cl != NULL, "cannot start player");
    sleep_ms(1500);
    ktest_assert(pixel(42, 50) == 0x00ebebeb, "player window active: %08x", pixel(42, 50));
    /* Window 560x300 at (40,60): the waveform canvas fills the middle. */
    int accent = count_color(40 + 10, 60 + 70, 540, 190, ACCENT);
    ktest_assert(accent > 500, "waveform drawn: %d accent pixels", accent);
    sleep_ms(1500);                     /* the 2 s chime ends */
    press_key(0x39);                    /* space: play again from the start */
    sleep_ms(400);
    alt_key(0x3e);
    int status = proc_reap(cl);
    ktest_assert(status == 0, "player status 0x%x", status);
    /* The packed 24-bit track, when the image carries it: 38 MB read and
     * resampled before the window appears. */
    const char *track = "/usr/share/sounds/58_Hammer_of_Justice.wav";
    struct inode *ino;
    if (vfs_lookup(track, &ino) == 0) {
        inode_put(ino);
        cl = proc_create_user("/bin/player", (char *const[]){ "player", (char *)track, NULL }, (char *const[]){ NULL },
                              &kernel_proc);
        ktest_assert(cl != NULL, "cannot start player");
        uint64_t t0 = timer_ms();
        while (pixel(72, 80) != 0x00ebebeb && timer_ms() - t0 < 10000)
            sleep_ms(100);
        kprintf("audio_player: track window after %lu ms\n", timer_ms() - t0);
        ktest_assert(pixel(72, 80) == 0x00ebebeb, "player window for the track: %08x", pixel(72, 80));
        ktest_assert(timer_ms() - t0 < 5000, "the window must not wait for the load");
        /* The loader thread reads and decodes 38 MB while the window answers. */
        while (count_color(70 + 10, 90 + 70, 540, 190, ACCENT) <= 500 && timer_ms() - t0 < 90000)
            sleep_ms(500);
        kprintf("audio_player: track loaded after %lu ms\n", timer_ms() - t0);
        accent = count_color(70 + 10, 90 + 70, 540, 190, ACCENT);
        ktest_assert(accent > 500, "track waveform drawn: %d accent pixels", accent);
        alt_key(0x3e);
        status = proc_reap(cl);
        ktest_assert(status == 0, "player status for the track 0x%x", status);
        kprintf("audio_player: 24-bit track ok\n");
    }
    stop_server(srv);
    stop_audiod(audiod);
    kprintf("audio_player: player ok\n");
}
KTEST_DEFINE("audio_player", test_audio_player);

/* The sequencer loads the demo pattern from the keyboard, shows it in
 * the grid and runs the transport. */
static void test_audio_sequencer(void)
{
    ktest_assert(fb_screen_present, "no framebuffer");
    struct proc *audiod = start_audiod();
    struct proc *srv = start_server();
    struct proc *cl = proc_create_user("/bin/sequencer", (char *const[]){ "sequencer", NULL },
                                       (char *const[]){ NULL }, &kernel_proc);
    ktest_assert(cl != NULL, "cannot start sequencer");
    sleep_ms(1500);
    ktest_assert(pixel(42, 50) == 0x00ebebeb, "sequencer window active: %08x", pixel(42, 50));
    int before = count_color(40 + 30, 60 + 50, 600, 200, ACCENT);
    press_key(0x20);                    /* D: the demo pattern */
    sleep_ms(400);
    int after = count_color(40 + 30, 60 + 50, 600, 200, ACCENT);
    ktest_assert(after > before + 200, "pattern cells drawn: %d -> %d accent pixels", before, after);
    press_key(0x39);                    /* space: play */
    sleep_ms(800);
    press_key(0x39);                    /* space: stop */
    sleep_ms(300);
    alt_key(0x3e);
    int status = proc_reap(cl);
    ktest_assert(status == 0, "sequencer status 0x%x", status);
    stop_server(srv);
    stop_audiod(audiod);
    kprintf("audio_sequencer: sequencer ok\n");
}
KTEST_DEFINE("audio_sequencer", test_audio_sequencer);

/* The panel's mixer applet lists the synthesizer's stream and sets the
 * master volume by a click on its bar. */
static void test_gui_mixer(void)
{
    ktest_assert(fb_screen_present, "no framebuffer");
    int sw = logical_w(), sh = logical_h();
    struct proc *audiod = start_audiod();
    struct proc *srv = start_server();
    struct proc *cl = proc_create_user("/bin/synth", (char *const[]){ "synth", NULL },
                                       (char *const[]){ NULL }, &kernel_proc);
    ktest_assert(cl != NULL, "cannot start synth");
    sleep_ms(1500);
    int cx = sw / 2, cy = sh / 2;
    int bx = sw - CLOCK_W - MIXER_BTN_W - 4;
    mouse_move_to(&cx, &cy, bx + MIXER_BTN_W / 2, sh - PANEL_H / 2, 0);
    mouse_click(1);
    sleep_ms(600);
    /* The popup: one master row and one stream row, above the button,
     * extending to the left. */
    int ph = 2 * MIXER_ROW_H + 2 * MIXER_PAD;
    int px = bx + MIXER_BTN_W - MIXER_W, py = sh - PANEL_H + 4 - ph;
    int bar_x = px + MIXER_PAD + 96, bar_w = MIXER_W - MIXER_PAD - 96 - MIXER_PAD;
    int master_y = py + MIXER_PAD + 12 + 4, stream_y = py + MIXER_PAD + MIXER_ROW_H + 12 + 4;
    ktest_assert(pixel(bar_x + bar_w - 20, master_y) == PANEL_ACCENT,
                 "master bar full: %08x", pixel(bar_x + bar_w - 20, master_y));
    ktest_assert(pixel(bar_x + bar_w - 20, stream_y) == PANEL_ACCENT,
                 "stream bar full: %08x", pixel(bar_x + bar_w - 20, stream_y));
    mouse_move_to(&cx, &cy, bar_x + bar_w / 2, master_y, 0);
    mouse_click(1);
    sleep_ms(400);
    ktest_assert(pixel(bar_x + bar_w - 20, master_y) == METER_BG,
                 "master bar half: %08x", pixel(bar_x + bar_w - 20, master_y));
    ktest_assert(pixel(bar_x + bar_w / 4, master_y) == PANEL_ACCENT,
                 "master bar filled to the left: %08x", pixel(bar_x + bar_w / 4, master_y));
    mouse_move_to(&cx, &cy, bx + MIXER_BTN_W / 2, sh - PANEL_H / 2, 0);
    mouse_click(1);
    sleep_ms(400);
    signal_send(cl, SIGTERM);
    proc_reap(cl);
    stop_server(srv);
    stop_audiod(audiod);
    kprintf("gui_mixer: mixer applet ok\n");
}
KTEST_DEFINE("gui_mixer", test_gui_mixer);
