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

/* Drag and drop through libgui (docs/design/dnd.md). The dndtest source
 * window drags three files in turn: into the dndtest target, which moves
 * the file into its folder after reading the paths before the drop (move
 * is preferred on one file system), into gedit, which opens it, and into
 * the terminal, which types its quoted path into the shell. */
static void drag_from_to(int *cx, int *cy, int x0, int y0, int x1, int y1)
{
    mouse_move_to(cx, cy, x0, y0, 0);
    sleep_ms(100);
    feed_packet(1, 0, 0);
    sleep_ms(300);
    mouse_move_to(cx, cy, x1, y1, 1);
    sleep_ms(800);                              /* the target reads the paths and answers */
    feed_packet(0, 0, 0);
    sleep_ms(800);
}

static void test_gui_dnd(void)
{
    ktest_assert(fb_screen_present, "no framebuffer");
    install_app("gedit");
    struct proc *srv = start_server();
    int cx = logical_w() / 2, cy = logical_h() / 2;
    /* Toplevels cascade by 30 pixels from the frame (40,30), and the
     * contents begin below the 36 pixel header bar: the source (240x160)
     * covers (40,66)-(280,226), the target (70,96)-(310,256). A press in
     * the source raises it, so drops land outside its rectangle. */
    struct proc *src = proc_create_user("/bin/dndtest",
        (char *const[]){ "dndtest", "source", "/tmp/dnd-a.txt", "/tmp/dnd-b.txt", "/tmp/dnd-c.txt", NULL },
        (char *const[]){ "PATH=/bin", NULL }, &kernel_proc);
    ktest_assert(src != NULL, "cannot start the drag source");
    sleep_ms(1500);
    struct proc *dst = proc_create_user("/bin/dndtest", (char *const[]){ "dndtest", "target", "/tmp/dndbox", NULL },
                                        (char *const[]){ "PATH=/bin", NULL }, &kernel_proc);
    ktest_assert(dst != NULL, "cannot start the drop target");
    sleep_ms(1500);
    drag_from_to(&cx, &cy, 50, 150, 295, 240);
    ktest_assert(wait_for("/tmp/dndbox/dnd-a.txt", true), "the dropped file is not in the target folder");
    ktest_assert(!exists("/tmp/dnd-a.txt"), "the dropped file was copied, not moved");
    /* gedit (toplevel 3) covers (100,126)-(780,606). */
    struct proc *ed = proc_create_user("/usr/bin/gedit", (char *const[]){ "gedit", NULL },
                                       (char *const[]){ "PATH=/bin", NULL }, &kernel_proc);
    ktest_assert(ed != NULL, "cannot start gedit");
    sleep_ms(2000);
    drag_from_to(&cx, &cy, 50, 150, 500, 400);
    ktest_assert(exists("/tmp/dnd-b.txt"), "the file dropped on gedit was moved");
    /* The terminal (toplevel 4) covers (130,156)-(792,587); the shell
     * receives "cp ", the dropped path and the rest of the line. */
    struct proc *term = proc_create_user("/bin/term", (char *const[]){ "term", NULL },
                                         (char *const[]){ "PATH=/bin", NULL }, &kernel_proc);
    ktest_assert(term != NULL, "cannot start term");
    sleep_ms(2000);
    type_line("cp ");
    drag_from_to(&cx, &cy, 50, 150, 600, 450);
    mouse_click(1);                             /* the terminal takes the keyboard again */
    sleep_ms(300);
    type_line("/tmp/dnd-copy.txt\n");
    ktest_assert(wait_for("/tmp/dnd-copy.txt", true), "the shell did not receive the dropped path");
    type_line("exit\n");
    proc_reap(term);
    signal_send(ed, SIGTERM);
    proc_reap(ed);
    signal_send(dst, SIGTERM);
    proc_reap(dst);
    signal_send(src, SIGTERM);
    proc_reap(src);
    stop_server(srv);
    kprintf("gui_dnd: ok\n");
}
KTEST_DEFINE("gui_dnd", test_gui_dnd);

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
