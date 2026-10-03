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
    /* Typing a name searches the folder, and Enter opens the first result
     * with gedit. */
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

/* The file chooser of libgui, opened from gedit.  Ctrl+O opens it in the
 * home folder, / starts the location entry, whose completion the typed path
 * overwrites, and Enter opens the file, which then heads the recent
 * list.  Ctrl+Shift+S opens it in save mode with the name of the file,
 * the typed name replaces the selected stem, and Enter saves the copy. */
static void read_text(const char *path, char *buf, size_t size)
{
    struct file *f;
    buf[0] = '\0';
    if (vfs_open(path, O_RDONLY, 0, &f) < 0)
        return;
    long n = file_read(f, buf, size - 1);
    file_put(f);
    buf[n > 0 ? n : 0] = '\0';
}

static void test_gui_filechooser(void)
{
    ktest_assert(fb_screen_present, "no framebuffer");
    install_app("gedit");
    vfs_unlink("/home/user/.local/share/recent-files");
    vfs_unlink("/home/user/desktop/copy.txt");
    struct proc *srv = start_server();
    struct proc *ed = proc_create_user("/usr/bin/gedit", (char *const[]){ "gedit", NULL },
                                       (char *const[]){ "PATH=/bin", "HOME=/home/user", NULL }, &kernel_proc);
    ktest_assert(ed != NULL, "cannot start gedit");
    sleep_ms(1500);
    type_ctrl('o');
    sleep_ms(1000);
    kprintf("gui_filechooser: open chooser shown\n");
    type_line("/home/user/desktop/readme.txt\n");
    sleep_ms(800);
    char buf[256];
    read_text("/home/user/.local/share/recent-files", buf, sizeof buf);
    ktest_assert(strcmp(buf, "/home/user/desktop/readme.txt\n") == 0, "recent list '%s'", buf);
    /* Ctrl+Shift+S opens Save as. */
    ps2kbd_feed_scancode(0x1d);
    ps2kbd_feed_scancode(0x2a);
    press_key(0x1f);
    ps2kbd_feed_scancode(0xaa);
    ps2kbd_feed_scancode(0x9d);
    sleep_ms(1000);
    kprintf("gui_filechooser: save chooser shown\n");
    type_line("copy\n");
    ktest_assert(wait_for("/home/user/desktop/copy.txt", true), "the copy was not saved");
    char original[256], copy[256];
    read_text("/home/user/desktop/readme.txt", original, sizeof original);
    read_text("/home/user/desktop/copy.txt", copy, sizeof copy);
    ktest_assert(copy[0] && strncmp(original, copy, 16) == 0, "the copy does not start like readme.txt");
    read_text("/home/user/.local/share/recent-files", buf, sizeof buf);
    ktest_assert(strcmp(buf, "/home/user/desktop/copy.txt\n/home/user/desktop/readme.txt\n") == 0, "recent list '%s'", buf);
    alt_key(0x3e);
    int status = proc_reap(ed);
    ktest_assert(status == 0, "gedit status 0x%x", status);
    stop_server(srv);
    vfs_unlink("/home/user/desktop/copy.txt");
    vfs_unlink("/home/user/.local/share/recent-files");
    kprintf("gui_filechooser: ok\n");
}
KTEST_DEFINE("gui_filechooser", test_gui_filechooser);
