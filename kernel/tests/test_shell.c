#include <tests/ktest.h>
#include <sched/user.h>
#include <sched/proc.h>
#include <drivers/ps2kbd.h>
#include <drivers/tty.h>
#include <mm/pmm.h>
#include <mm/swap.h>
#include <console.h>
#include <fs/vfs.h>
#include <lib/string.h>
#include <drivers/timer.h>
#include <sync/rcu.h>
#include <drivers/fbcon.h>

struct shell_reclaim {
    struct rcu_head head;
    bool done;
};

static void shell_reclaim_done(struct rcu_head *head)
{
    struct shell_reclaim *reclaim = container_of(head, struct shell_reclaim, head);
    __atomic_store_n(&reclaim->done, true, __ATOMIC_RELEASE);
}

/* Process teardown defers VMA freeing through RCU. Measure after those
 * callbacks complete so the assertion counts leaks, not pending frees. */
static void shell_memory_drain(void)
{
    struct shell_reclaim reclaim = {0};
    rcu_call(&reclaim.head, shell_reclaim_done);
    while (!__atomic_load_n(&reclaim.done, __ATOMIC_ACQUIRE))
        sleep_ms(1);
    swap_drain();
}

/* M10: type a session into the keyboard buffer, then run the shell on it.
 * The typed commands exercise builtins, PATH lookup, argument passing and
 * the error path; the last command makes the shell exit with status 3. */
static uint8_t scancode_for(char c)
{
    static const char row1[] = "1234567890-=";
    static const char row2[] = "qwertyuiop[]";
    static const char row3[] = "asdfghjkl;'`";
    static const char row4[] = "\\zxcvbnm,./";
    for (int i = 0; row1[i]; i++) if (row1[i] == c) return (uint8_t)(0x02 + i);
    for (int i = 0; row2[i]; i++) if (row2[i] == c) return (uint8_t)(0x10 + i);
    for (int i = 0; row3[i]; i++) if (row3[i] == c) return (uint8_t)(0x1e + i);
    for (int i = 0; row4[i]; i++) if (row4[i] == c) return (uint8_t)(0x2b + i);
    if (c == ' ') return 0x39;
    if (c == '\n') return 0x1c;
    if (c == '\t') return 0x0f;
    return 0;
}

void type_ctrl(char c)
{
    ps2kbd_feed_scancode(0x1d);
    uint8_t code = scancode_for(c);
    ps2kbd_feed_scancode(code);
    ps2kbd_feed_scancode(code | 0x80);
    ps2kbd_feed_scancode(0x1d | 0x80);
}

void type_line(const char *s)
{
    for (; *s; s++) {
        char c = *s;
        bool shift = false;
        static const char shifted[] = "<,>.|\\$4&7\"'_-(9)0:;*8?/!1{[}]~`#3%5^6+=@2";
        for (const char *m = shifted; *m; m += 2) {
            if (m[0] == c) {
                c = m[1];
                shift = true;
                break;
            }
        }
        if (c >= 'A' && c <= 'Z') {
            c = (char)(c - 'A' + 'a');
            shift = true;
        }
        if (shift)
            ps2kbd_feed_scancode(0x2a);
        uint8_t code = scancode_for(c);
        ktest_assert(code != 0, "no scancode for '%c'", *s);
        ps2kbd_feed_scancode(code);
        ps2kbd_feed_scancode(code | 0x80);
        if (shift)
            ps2kbd_feed_scancode(0x2a | 0x80);
    }
}

static void test_shell(void)
{
    type_line("help\n");
    type_line("echo hello from sh\n");
    type_line("pwd\n");
    type_line("cd /bin\n");
    type_line("pwd\n");
    type_line("hello one two\n");
    type_line("nosuchprogram\n");
    type_line("cd /nowhere\n");
    type_line("cd\n");
    type_line("exit 3\n");

    struct pmm_stats before, after;
    shell_memory_drain();
    pmm_get_stats(&before);
    char *const argv[] = { "sh", NULL };
    char *const envp[] = { "PATH=/bin", NULL };
    struct proc *p = proc_create_user("/bin/sh", argv, envp, &kernel_proc);
    ktest_assert(p != NULL, "cannot start /bin/sh");
    int status = proc_reap(p);
    kprintf("sh exited with status 0x%x\n", status);
    ktest_assert(status == PROC_STATUS_EXITED(3), "sh status 0x%x", status);
    /* The release of the file mappings of a program takes more than one
     * grace period: poll until the count settles. */
    for (int i = 0; i < 100; i++) {
        shell_memory_drain();
        pmm_get_stats(&after);
        if (after.free_pages == before.free_pages)
            break;
        sleep_ms(2);
    }
    ktest_assert(after.free_pages == before.free_pages, "leaked %ld pages",
                 (long)before.free_pages - (long)after.free_pages);
}
KTEST_DEFINE("shell", test_shell);

static void type_key(uint8_t code)
{
    ps2kbd_feed_scancode(0xe0);
    ps2kbd_feed_scancode(code);
    ps2kbd_feed_scancode(0xe0);
    ps2kbd_feed_scancode(code | 0x80);
}

static void lineedit_wait(void)
{
    for (int i = 0; i < 100 && (tty_get_lflag(&console_tty) & ICANON); i++)
        sleep_ms(50);
    ktest_assert(!(tty_get_lflag(&console_tty) & ICANON), "line editor did not enter raw mode");
}

static void test_lineedit(void)
{
    struct proc *p = proc_create_user("/bin/sh", (char *const[]){ "sh", NULL },
                                      (char *const[]){ "PATH=/bin", NULL }, &kernel_proc);
    ktest_assert(p != NULL, "cannot start shell");
    lineedit_wait();
    type_line("cho hello");
    type_key(0x47); /* Home */
    type_line("e\n");
    sleep_ms(200);
    lineedit_wait();
    type_line("hexd\t /dev/null\n");
    sleep_ms(300);
    lineedit_wait();
    type_key(0x48); /* Up */
    type_line("\n");
    sleep_ms(300);
    lineedit_wait();
    type_line("exit 12\n");
    int status = proc_reap(p);
    ktest_assert(status == PROC_STATUS_EXITED(12), "lineedit status 0x%x", status);
    ktest_assert(tty_get_lflag(&console_tty) & ICANON, "line editor left raw mode");
    kprintf("lineedit: completion, history and Home editing passed\n");
}
KTEST_DEFINE("lineedit", test_lineedit);

static void screen_line(uint32_t row, char *text, uint16_t columns)
{
    for (uint16_t col = 0; col < columns; col++)
        ktest_assert(fbcon_get_cell(col, row, &text[col], NULL), "missing console cell");
    text[columns] = 0;
    for (size_t n = columns; n && text[n - 1] == ' '; n--)
        text[n - 1] = 0;
}

static void test_lineedit_screen(void)
{
    struct proc *p = proc_create_user("/bin/sh", (char *const[]){ "sh", NULL },
        (char *const[]){ "PATH=/bin", "TERM=minios", "USER=user", NULL }, &kernel_proc);
    ktest_assert(p != NULL, "cannot start shell");
    lineedit_wait();
    type_line("printf '\\033[2J\\033[H'\n");
    sleep_ms(300);
    lineedit_wait();
    for (int repetition = 0; repetition < 3; repetition++) {
        type_line("thisisnotacommand\n");
        sleep_ms(300);
        lineedit_wait();
    }
    console_flush();
    uint16_t columns, rows;
    fbcon_get_size(&columns, &rows);
    char text[257];
    ktest_assert(columns <= 256 && rows > 7, "console geometry");
    for (uint32_t row = 0; row < 6; row++) {
        screen_line(row, text, columns);
        const char *expected = row % 2 == 0 ? "user:/ $ thisisnotacommand"
                                            : "thisisnotacommand: No such file or directory";
        ktest_assert(!strcmp(text, expected), "screen row %u: '%s' expected '%s'", row, text, expected);
    }
    screen_line(6, text, columns);
    ktest_assert(!strcmp(text, "user:/ $"), "final prompt contains stale text: '%s'", text);
    type_line("exit 0\n");
    ktest_assert(proc_reap(p) == 0, "shell exit");
    kprintf("lineedit_screen: repeated commands render without duplicate or stale text\n");
}
KTEST_DEFINE("lineedit_screen", test_lineedit_screen);

/* The interactive prompt of /bin/lua edits lines with libedit through
 * the readline hooks of lua.c (user/lua/lreadline.c). Each result is
 * printed in a form the echoed input does not contain: Tab completes
 * string.up to string.upper, Up recalls the counter line, Ctrl+C
 * discards a line, and Ctrl+D on an empty line ends the interpreter.
 * /etc/tests/luaprompt.lua then checks the history file. */
static void test_lua_prompt(void)
{
    struct proc *p = proc_create_user("/bin/lua", (char *const[]){ "lua", NULL },
                                      (char *const[]){ "PATH=/bin", "HOME=/tmp", NULL }, &kernel_proc);
    ktest_assert(p != NULL, "lua did not start");
    lineedit_wait();
    type_line("print(\"answer \" .. 6 * 7)\n");
    sleep_ms(300);
    lineedit_wait();
    type_line("print(\"up \" .. string.up\t(\"ok\"))\n");
    sleep_ms(300);
    lineedit_wait();
    type_line("n = (n or 0) + 1; print(\"run \" .. n)\n");
    sleep_ms(300);
    lineedit_wait();
    type_key(0x48); /* Up recalls the counter line. */
    type_line("\n");
    sleep_ms(300);
    lineedit_wait();
    type_line("error(\"discarded\"");
    type_ctrl('c');
    sleep_ms(300);
    lineedit_wait();
    type_line("print(\"after \" .. n)\n");
    sleep_ms(300);
    lineedit_wait();
    type_ctrl('d');
    int status = proc_reap(p);
    ktest_assert(status == 0, "lua exited with status 0x%x", status);
    ktest_assert(tty_get_lflag(&console_tty) & ICANON, "the line editor left the terminal in raw mode");
    p = proc_create_user("/bin/lua", (char *const[]){ "lua", "/etc/tests/luaprompt.lua", NULL },
                         (char *const[]){ "PATH=/bin", "HOME=/tmp", NULL }, &kernel_proc);
    ktest_assert(p != NULL, "lua did not start for the history check");
    status = proc_reap(p);
    ktest_assert(status == 0, "the history check exited with status 0x%x", status);
    kprintf("lua_prompt: completion, history and Ctrl+C passed\n");
}
KTEST_DEFINE("lua_prompt", test_lua_prompt);

/* M11: pipelines, redirections and directory listing through the shell. */
static void test_pipes(void)
{
    type_line("ls /dev\n");
    type_line("cat < /etc/motd\n");
    type_line("cat /etc/motd | wc\n");
    type_line("echo redirected to the console > /dev/console\n");
    type_line("echo dropped > /dev/null\n");
    type_line("cat /etc/motd | cat | cat | wc -l > /dev/null\n");
    type_line("exit 5\n");

    struct pmm_stats before, after;
    shell_memory_drain();
    pmm_get_stats(&before);
    char *const argv[] = { "sh", NULL };
    char *const envp[] = { "PATH=/bin", NULL };
    struct proc *p = proc_create_user("/bin/sh", argv, envp, &kernel_proc);
    ktest_assert(p != NULL, "cannot start /bin/sh");
    int status = proc_reap(p);
    kprintf("sh exited with status 0x%x\n", status);
    ktest_assert(status == PROC_STATUS_EXITED(5), "sh status 0x%x", status);
    /* The release of the file mappings of a program takes more than one
     * grace period: poll until the count settles. */
    for (int i = 0; i < 100; i++) {
        shell_memory_drain();
        pmm_get_stats(&after);
        if (after.free_pages == before.free_pages)
            break;
        sleep_ms(2);
    }
    ktest_assert(after.free_pages == before.free_pages, "leaked %ld pages",
                 (long)before.free_pages - (long)after.free_pages);
}
KTEST_DEFINE("pipes", test_pipes);

/* M15: control C terminates the foreground program, ps lists processes,
 * kill sends signals. */
static void test_ctrlc(void)
{
    type_line("cat\n");
    type_line("this line reaches cat\n");
    struct proc *p = proc_create_user("/bin/sh", (char *const[]){ "sh", NULL },
                                      (char *const[]){ "PATH=/bin", NULL }, &kernel_proc);
    ktest_assert(p != NULL, "cannot start /bin/sh");
    /* Let the shell start cat and cat echo its line, then interrupt it
     * while it blocks on the console. */
    sleep_ms(1500);
    type_ctrl('c');
    sleep_ms(500);
    type_line("ps\n");
    type_line("kill -2 99999\n");
    type_line("exit 4\n");
    int status = proc_reap(p);
    kprintf("sh exited with status 0x%x\n", status);
    ktest_assert(status == PROC_STATUS_EXITED(4), "sh status 0x%x", status);
}
KTEST_DEFINE("ctrlc", test_ctrlc);

/* M15: the shutdown utility signals init, which performs the orderly
 * power off through reboot(). The host checks the image afterwards. */
static void test_shutdown_cmd(void)
{
    /* init runs login on the console. root has no password and chooses
     * one before the shell starts. */
    type_line("root\n");
    type_line("rootpw\n");
    type_line("rootpw\n");
    type_line("echo before shutdown > /marker.txt\n");
    type_line("cat /marker.txt\n");
    type_line("shutdown\n");
    struct proc *p = proc_create_user("/bin/init", (char *const[]){ "/bin/init", NULL },
                                      (char *const[]){ "PATH=/bin", NULL }, &kernel_proc);
    ktest_assert(p != NULL, "cannot start /bin/init");
    proc_set_init(p);
    int status = proc_reap(p);
    ktest_fail("init exited with status 0x%x", status);
}
KTEST_DEFINE("shutdown_cmd", test_shutdown_cmd);

/* Init supervises the entries of its configuration and answers initctl:
 * a service added by a reload is stopped, started and restarted, a
 * command that keeps failing is given up after five quick exits, a
 * removed entry disappears, and the power off request goes through the
 * same orderly shutdown as the signal. */
static void test_initctl(void)
{
    /* init runs login on the console. root has no password and chooses
     * one before the shell starts. */
    type_line("root\n");
    type_line("rootpw\n");
    type_line("rootpw\n");
    type_line("cp /etc/init.conf /tmp/init.conf\n");
    type_line("echo 'service spin sleep 1000' >> /tmp/init.conf\n");
    type_line("initctl reload /tmp/init.conf\n");
    type_line("initctl status spin\n");
    type_line("initctl stop spin\n");
    type_line("initctl status spin\n");
    type_line("initctl start spin\n");
    type_line("cp /etc/init.conf /tmp/init2.conf\n");
    type_line("echo 'service spin test 1 = 2' >> /tmp/init2.conf\n");
    type_line("initctl reload /tmp/init2.conf\n");
    type_line("initctl restart spin\n");
    type_line("sleep 6\n");
    type_line("initctl list\n");
    type_line("initctl reload /etc/init.conf\n");
    type_line("initctl status spin\n");
    type_line("initctl nosuch\n");
    type_line("initctl poweroff\n");
    struct proc *p = proc_create_user("/bin/init", (char *const[]){ "/bin/init", NULL },
                                      (char *const[]){ "PATH=/bin", NULL }, &kernel_proc);
    ktest_assert(p != NULL, "cannot start /bin/init");
    proc_set_init(p);
    int status = proc_reap(p);
    ktest_fail("init exited with status 0x%x", status);
}
KTEST_DEFINE("initctl", test_initctl);

/* M16: quoting, variables, lists and background jobs typed into an
 * interactive shell. */
static void test_shell2(void)
{
    type_line("x=/bin\n");
    type_line("echo 'literal $HOME' and expanded $x\n");
    type_line("sleep 0.5 &\n");
    type_line("wait\n");
    type_line("echo after wait\n");
    type_line("true && echo first ok || echo not shown\n");
    type_line("echo \"a b\" c | wc -w\n");
    type_line("exit 9\n");
    struct proc *p = proc_create_user("/bin/sh", (char *const[]){ "sh", NULL },
                                      (char *const[]){ "PATH=/bin", NULL }, &kernel_proc);
    ktest_assert(p != NULL, "cannot start /bin/sh");
    int status = proc_reap(p);
    kprintf("sh exited with status 0x%x\n", status);
    ktest_assert(status == PROC_STATUS_EXITED(9), "sh status 0x%x", status);
}
KTEST_DEFINE("shell2", test_shell2);

/* Foreground process groups can be stopped with control Z, resumed in
 * the background, and brought back to the terminal with fg. */
static void test_jobcontrol(void)
{
    struct proc *p = proc_create_user("/bin/sh", (char *const[]){ "sh", NULL },
                                      (char *const[]){ "PATH=/bin", NULL }, &kernel_proc);
    ktest_assert(p != NULL, "cannot start /bin/sh");
    sleep_ms(500);
    type_line("yes > /dev/null\n");
    sleep_ms(500);
    type_ctrl('z');
    sleep_ms(300);
    type_line("jobs\n");
    sleep_ms(200);
    type_line("bg\n");
    sleep_ms(200);
    type_line("jobs\n");
    sleep_ms(200);
    type_line("fg\n");
    sleep_ms(300);
    type_ctrl('c');
    sleep_ms(300);
    type_line("exit 6\n");
    int status = proc_reap(p);
    kprintf("sh exited with status 0x%x\n", status);
    ktest_assert(status == PROC_STATUS_EXITED(6), "sh status 0x%x", status);
}
KTEST_DEFINE("jobcontrol", test_jobcontrol);

/* M16: the editor in raw keyboard mode. Two lines are queued before it
 * starts; cursor up, Home, an insertion, save and quit follow once it
 * runs. */
static void test_editor(void)
{
    type_line("hello\nworld");
    struct proc *p = proc_create_user("/bin/edit", (char *const[]){ "edit", "/edited.txt", NULL },
                                      (char *const[]){ "PATH=/bin", NULL }, &kernel_proc);
    ktest_assert(p != NULL, "cannot start /bin/edit");
    /* Cursor keys only exist in raw mode, so wait until the editor has
     * switched the terminal. */
    for (int i = 0; i < 100 && (tty_get_lflag(&console_tty) & ICANON); i++)
        sleep_ms(50);
    ktest_assert(!(tty_get_lflag(&console_tty) & ICANON), "editor did not enter raw mode");
    ps2kbd_feed_scancode(0xe0); ps2kbd_feed_scancode(0x48);   /* up */
    ps2kbd_feed_scancode(0xe0); ps2kbd_feed_scancode(0xc8);
    ps2kbd_feed_scancode(0xe0); ps2kbd_feed_scancode(0x47);   /* home */
    ps2kbd_feed_scancode(0xe0); ps2kbd_feed_scancode(0xc7);
    type_line("1");
    type_ctrl('s');
    type_ctrl('q');
    int status = proc_reap(p);
    ktest_assert(status == 0, "edit status 0x%x", status);
    struct file *f;
    ktest_assert(vfs_open("/edited.txt", O_RDONLY, 0, &f) == 0, "open /edited.txt");
    char buf[64];
    long n = file_read(f, buf, sizeof buf - 1);
    file_put(f);
    ktest_assert(n > 0, "read");
    buf[n] = '\0';
    ktest_assert(strcmp(buf, "1hello\nworld\n") == 0, "content '%s'", buf);
    ktest_assert(tty_get_lflag(&console_tty) == (ICANON | ECHO | ISIG), "terminal mode not restored");
    kprintf("edited file matches\n");
}
KTEST_DEFINE("editor", test_editor);
