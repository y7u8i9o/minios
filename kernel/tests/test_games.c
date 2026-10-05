#include <tests/ktest.h>
#include <sched/user.h>
#include <sched/proc.h>
#include <drivers/ps2kbd.h>
#include <drivers/tty.h>
#include <drivers/timer.h>
#include <fs/vfs.h>
#include <lib/string.h>
#include <minios/abi.h>
#include <console.h>

/* The interactive programs added after M18: each one is started through
 * the shell with its output redirected to a file, driven with keys once
 * it has switched the console to raw mode, quit with q, and its final
 * line is checked. The console mode must be restored afterwards. */

static bool contains(const char *hay, const char *needle)
{
    size_t n = strlen(needle);
    for (const char *p = hay; *p; p++)
        if (strncmp(p, needle, n) == 0)
            return true;
    return false;
}

static void wait_raw(void)
{
    for (int i = 0; i < 100 && (tty_get_lflag(&console_tty) & ICANON); i++)
        sleep_ms(50);
    ktest_assert(!(tty_get_lflag(&console_tty) & ICANON), "program did not enter raw mode");
}

static void arrow(uint8_t code)
{
    ps2kbd_feed_scancode(0xe0);
    ps2kbd_feed_scancode(code);
    ps2kbd_feed_scancode(0xe0);
    ps2kbd_feed_scancode(code | 0x80);
}

static void read_file(const char *path, char *buf, size_t size)
{
    struct file *f;
    ktest_assert(vfs_open(path, O_RDONLY, 0, &f) == 0, "open %s", path);
    size_t n = 0;
    for (;;) {
        long r = file_read(f, buf + n, size - 1 - n);
        if (r <= 0 || n + (size_t)r >= size - 1)
            break;
        n += (size_t)r;
    }
    file_put(f);
    buf[n] = '\0';
}

static struct proc *start(const char *command)
{
    struct proc *p = proc_create_user("/bin/sh", (char *const[]){ "sh", "-c", (char *)command, NULL },
                                      (char *const[]){ "PATH=/bin", NULL }, &kernel_proc);
    ktest_assert(p != NULL, "cannot start sh -c '%s'", command);
    return p;
}

static void test_games(void)
{
    char buf[4096];

    struct proc *p = start("snake > /snake.out");
    wait_raw();
    arrow(0x48);            /* up */
    ktest_wait_idle(400);
    arrow(0x4d);            /* right */
    ktest_wait_idle(400);
    type_line("q");
    ktest_assert(proc_reap(p) == 0, "snake status");
    ktest_assert(tty_get_lflag(&console_tty) & ICANON, "snake left the console in raw mode");
    read_file("/snake.out", buf, sizeof buf);
    ktest_assert(contains(buf, "Quit, score") || contains(buf, "Game over"),
                 "snake did not print its final line");
    kprintf("games: snake ran and quit\n");

    p = start("2048 > /2048.out");
    wait_raw();
    arrow(0x4b);            /* left */
    ktest_wait_idle(200);
    arrow(0x50);            /* down */
    ktest_wait_idle(200);
    type_line("q");
    ktest_assert(proc_reap(p) == 0, "2048 status");
    ktest_assert(tty_get_lflag(&console_tty) & ICANON, "2048 left the console in raw mode");
    read_file("/2048.out", buf, sizeof buf);
    ktest_assert(contains(buf, "2048    score"), "2048 did not draw its board");
    kprintf("games: 2048 ran and quit\n");

    p = start("matrix > /matrix.out");
    wait_raw();
    ktest_wait_idle(500);
    type_line("x");
    ktest_assert(proc_reap(p) == 0, "matrix status");
    ktest_assert(tty_get_lflag(&console_tty) & ICANON, "matrix left the console in raw mode");
    read_file("/matrix.out", buf, sizeof buf);
    ktest_assert(contains(buf, "\033[2J"), "matrix did not draw");
    kprintf("games: matrix ran until a key\n");

    p = start("sl > /sl.out && life -g 3 -w 10 -h 6 > /life.out");
    ktest_assert(proc_reap(p) == 0, "sl and life status");
    read_file("/life.out", buf, sizeof buf);
    ktest_assert(contains(buf, "generation 3,"), "life did not reach generation 3");
    kprintf("games: sl and life finished\n");

    vfs_unlink("/snake.out");
    vfs_unlink("/2048.out");
    vfs_unlink("/matrix.out");
    vfs_unlink("/sl.out");
    vfs_unlink("/life.out");
}
KTEST_DEFINE("games", test_games);
