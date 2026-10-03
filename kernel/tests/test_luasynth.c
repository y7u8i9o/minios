/* Both synthesizers coexist as packages. Exercise the Lua instrument with
 * real three- and eight-note keyboard chords; the host verifies stereo WAV. */
#include <tests/ktest.h>
#include <drivers/fbdev.h>
#include <drivers/timer.h>
#include <sched/user.h>
#include <sched/proc.h>
#include <ipc/signal.h>
#include <lib/cmdline.h>
#include <console.h>
#include "gui_helpers.h"

static void test_gui_luasynth(void)
{
    install_app("synth");
    install_app("luasynth");
    struct proc *daemon = proc_create_user("/bin/audiod", (char *const[]){ "audiod", NULL },
                                          (char *const[]){ NULL }, &kernel_proc);
    ktest_assert(daemon != NULL, "cannot start audiod");
    sleep_ms(300);
    struct proc *srv = start_server();
    struct proc *cl = proc_create_user("/usr/bin/luasynth", (char *const[]){ "luasynth", NULL },
                                      (char *const[]){ "HOME=/home", "PATH=/bin:/usr/bin", NULL }, &kernel_proc);
    ktest_assert(cl != NULL, "cannot start Lua Synthesizer");
    int found = 0;
    for (int attempt = 0; attempt < 60 && !found; attempt++) {
        sleep_ms(100);
        for (int y = 100; y < logical_h() - 40 && !found; y += 8)
            for (int x = 60; x < logical_w() - 40; x += 8)
                if (pixel(x, y) == 0x00151d2b) { found = 1; break; }
    }
    ktest_assert(found, "Lua synthesizer scope not on screen");
    sleep_ms(500);
    ps2kbd_feed_scancode(0x2c); /* Z: C4 */
    ps2kbd_feed_scancode(0x2e); /* C: E4 */
    ps2kbd_feed_scancode(0x30); /* B: G4 */
    sleep_ms(1200);
    ps2kbd_feed_scancode(0xac);
    ps2kbd_feed_scancode(0xae);
    ps2kbd_feed_scancode(0xb0);
    sleep_ms(500);
    const uint8_t chord[] = {0x2c, 0x2d, 0x2e, 0x2f, 0x30, 0x31, 0x32, 0x10};
    for (unsigned i = 0; i < sizeof chord; i++)
        ps2kbd_feed_scancode(chord[i]);
    sleep_ms(1500);
    for (unsigned i = 0; i < sizeof chord; i++)
        ps2kbd_feed_scancode(chord[i] | 0x80);
    sleep_ms(500);
    alt_key(0x3e);             /* close via the normal window action */
    int status = proc_reap(cl);
    ktest_assert(status == 0, "Lua synthesizer status 0x%x", status);
    stop_server(srv);
    signal_send(daemon, SIGTERM);
    status = proc_reap(daemon);
    ktest_assert(status == 0, "audiod status 0x%x", status);
    kprintf("gui_luasynth: polyphonic instrument ok\n");
}
KTEST_DEFINE("gui_luasynth", test_gui_luasynth);

/* Diagnostic matrix: precomputed PCM, live DSP, and GUI rendering share
 * identical buffers and audio service. Metrics are emitted by Lua. */
static void test_luasynth_profile(void)
{
    char mode[24] = "matrix";
    cmdline_lookup("profile_mode", mode, sizeof mode);
    install_app("luasynth");
    struct proc *daemon = proc_create_user("/bin/audiod", (char *const[]){ "audiod", NULL },
                                          (char *const[]){ NULL }, &kernel_proc);
    ktest_assert(daemon != NULL, "cannot start audiod");
    sleep_ms(300);
    struct proc *srv = start_server();
    struct proc *cl = proc_create_user("/bin/lua", (char *const[]){ "lua", "/etc/tests/luasynth-profile.lua", "/usr/share/apps/luasynth/", mode, NULL },
                                      (char *const[]){ "HOME=/home", "PATH=/bin:/usr/bin", NULL }, &kernel_proc);
    ktest_assert(cl != NULL, "cannot start audio profile");
    int status = proc_reap(cl);
    stop_server(srv);
    signal_send(daemon, SIGTERM);
    proc_reap(daemon);
    ktest_assert(status == 0, "audio profile status 0x%x", status);
}
KTEST_DEFINE("luasynth_profile", test_luasynth_profile);
