/* The Files program: a folder is created, renamed and deleted through
 * the keyboard (Ctrl+N, F2, Delete with its confirmation), a text file
 * is opened through its type handler by typing its name, and the
 * windows close with Alt+F4. */
#include <tests/ktest.h>
#include <sched/user.h>
#include <sched/proc.h>
#include <ipc/signal.h>
#include <drivers/timer.h>
#include <fs/vfs.h>
#include <lib/string.h>
#include <console.h>
#include "gui_helpers.h"

static bool exists(const char *path)
{
    struct inode *n = NULL;
    if (vfs_lookup(path, &n) < 0)
        return false;
    inode_put(n);
    return true;
}

static bool wait_for(const char *path, bool present)
{
    for (int i = 0; i < 100 && exists(path) != present; i++)
        sleep_ms(50);
    return exists(path) == present;
}

static void test_gui_files(void)
{
    ktest_assert(fb_screen_present, "no framebuffer");
    /* The text editor that opens the file ships as a package. */
    install_app("gedit");
    struct proc *srv = start_server();
    struct proc *files = proc_create_user("/bin/files", (char *const[]){ "files", "/home/user/desktop", NULL },
                                          (char *const[]){ "PATH=/bin", NULL }, &kernel_proc);
    ktest_assert(files != NULL, "cannot start files");
    sleep_ms(1500);
    ktest_assert(pixel(42, 50) == 0x00ebebeb, "files window has an active title bar: %08x", pixel(42, 50));
    /* Ctrl+N opens the new folder prompt; Ctrl+A replaces its text. */
    type_ctrl('n');
    sleep_ms(500);
    type_ctrl('a');
    type_line("testdir\n");
    ktest_assert(wait_for("/home/user/desktop/testdir", true), "the folder was not created");
    sleep_ms(500);
    /* F2 renames the folder, which the program selected. */
    press_key(0x3c);
    sleep_ms(500);
    type_ctrl('a');
    type_line("renamed\n");
    ktest_assert(wait_for("/home/user/desktop/renamed", true), "the folder was not renamed");
    ktest_assert(!exists("/home/user/desktop/testdir"), "the old folder name remains");
    sleep_ms(500);
    /* Delete asks for confirmation; Enter chooses the Delete button. */
    ps2kbd_feed_scancode(0xe0);
    ps2kbd_feed_scancode(0x53);
    ps2kbd_feed_scancode(0xe0);
    ps2kbd_feed_scancode(0xd3);
    sleep_ms(500);
    type_line("\n");
    ktest_assert(wait_for("/home/user/desktop/renamed", false), "the folder was not deleted");
    sleep_ms(500);
    /* Typing a name selects the entry; Enter opens it with gedit. */
    type_line("readme\n");
    sleep_ms(2000);
    alt_key(0x3e);                              /* Alt+F4: the editor */
    sleep_ms(500);
    alt_key(0x3e);                              /* Alt+F4: the file manager */
    int status = proc_reap(files);
    ktest_assert(status == 0, "files status 0x%x", status);
    stop_server(srv);
    kprintf("gui_files: ok\n");
}
KTEST_DEFINE("gui_files", test_gui_files);
