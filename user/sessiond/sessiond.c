/* sessiond: the session manager of the graphical session
 * (docs/design/sessiond.md).
 *
 *     sessiond
 *
 * X12 detects the events of a session and reports them through the
 * session_manager interface (protocol/session.xml). sessiond decides the
 * actions:
 *
 * - Super+L, the idle time of the setting lock_timeout and a key press in
 *   a locked session without a locker start the screen locker /bin/lock.
 * - Print Screen, Shift+Print Screen and Super+Shift+3, 4 and 5 start
 *   /bin/screenshot.
 * - A client that stops answering pings gets a dialog with Wait and
 *   Force quit. Wait hides the dialog for fifteen seconds. Force quit ends
 *   the process of the client and disconnects the client.
 *
 * The greeter starts sessiond as root next to X12 and restarts it. startgui
 * starts sessiond when startgui itself starts X12. X12 admits the session
 * manager only from root or from the user that runs X12. sessiond starts
 * the programs as the session user. Without a session user, sessiond
 * starts no locker, and the screenshot program runs as the user of
 * sessiond.
 *
 * sessiond logs one line per action on standard output, and the boot tests
 * read these lines. */
#include <errno.h>
#include <pwd.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <gui/app.h>
#include <gui/client.h>
#include <gui/i18n.h>
#include <gui/keymap.h>
#include <minios/account.h>
#include <minios/conf.h>
#include <minios/input.h>
#include "session-client.h"

#define LOCKER_PATH "/bin/lock"
#define SCREENSHOT_PATH "/bin/screenshot"
#define WAIT_MS 15000
#define MAX_HUNG 16

/* A client that does not answer the pings of X12. */
struct hung {
    uint32_t number, pid;
    char title[64];
    long wait_until;                    /* uptime ms until which the dialog is hidden, 0 for none */
};

static struct app *app;
static struct wire_proxy *manager;
static int session_uid = -1;
static pid_t locker_pid;
static struct hung hung[MAX_HUNG];
static int nhung;
static struct widget *dialog;           /* the dialog of the entry dialog_number, or NULL */
static uint32_t dialog_number;
static struct timer *wait_timer, *show_timer;

static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    printf("sessiond: ");
    vprintf(fmt, ap);
    printf("\n");
    fflush(stdout);
    va_end(ap);
}

/* ---- programs ---- */

/* Start argv as the session user and return the pid, or -1. Without a
 * session user, the program runs as the user of sessiond, unless
 * need_session is 1. The child waits until the parent has run before_exec
 * with the pid. The order lets X12 learn the pid of a locker before the
 * locker connects. */
static pid_t start_program(char *const argv[], int need_session, void (*before_exec)(pid_t pid))
{
    if (session_uid < 0 && need_session) {
        say("no session for %s", argv[0]);
        return -1;
    }
    uid_t uid = session_uid >= 0 ? (uid_t)session_uid : getuid();
    struct passwd *pw = getpwuid(uid);
    if (!pw) {
        say("no account of uid %d", (int)uid);
        return -1;
    }
    char name[64], home[256];
    strlcpy(name, pw->pw_name, sizeof name);
    strlcpy(home, pw->pw_dir[0] ? pw->pw_dir : "/", sizeof home);
    gid_t gid = pw->pw_gid;
    int gate[2];
    if (pipe(gate) < 0)
        return -1;
    pid_t pid = fork();
    if (pid < 0) {
        say("cannot start %s: %s", argv[0], strerror(errno));
        close(gate[0]);
        close(gate[1]);
        return -1;
    }
    if (pid == 0) {
        close(gate[1]);
        char go;
        if (read(gate[0], &go, 1) != 1)
            _exit(125);
        close(gate[0]);
        if (getuid() != uid && account_become(name, (unsigned)uid, gid) < 0)
            _exit(126);
        if (chdir(home) < 0)
            chdir("/");
        setenv("HOME", home, 1);
        setenv("USER", name, 1);
        setenv("LOGNAME", name, 1);
        conf_export_locale();
        execv(argv[0], argv);
        _exit(127);
    }
    close(gate[0]);
    if (before_exec)
        before_exec(pid);
    write(gate[1], "g", 1);
    close(gate[1]);
    return pid;
}

static void report_locker(pid_t pid)
{
    session_manager_set_locker(manager, (uint32_t)pid);
    gui_flush();
}

static void reap(void *arg);

static void start_locker(const char *reason)
{
    reap(NULL);
    if (locker_pid > 0) {
        say("locker %d runs already", (int)locker_pid);
        return;
    }
    char *const argv[] = { LOCKER_PATH, NULL };
    pid_t pid = start_program(argv, 1, report_locker);
    if (pid > 0) {
        locker_pid = pid;
        say("locker %d started (%s)", (int)pid, reason);
    }
}

static void start_screenshot(const char *option)
{
    char *const argv[] = { SCREENSHOT_PATH, (char *)option, NULL };
    if (start_program(argv, 0, NULL) > 0)
        say("screenshot started");
}

/* Collect the programs that ended. The locker may start again after its
 * exit. */
static void reap(void *arg)
{
    (void)arg;
    int status;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        if (pid == locker_pid) {
            say("locker %d exited with status %d", (int)pid, WIFEXITED(status) ? WEXITSTATUS(status) : -1);
            locker_pid = 0;
        }
    }
}

/* ---- the dialog of an unresponsive client ---- */

static void show_next(void);

static struct hung *find_hung(uint32_t number)
{
    for (int i = 0; i < nhung; i++)
        if (hung[i].number == number)
            return &hung[i];
    return NULL;
}

static void remove_hung(uint32_t number)
{
    struct hung *h = find_hung(number);
    if (h)
        *h = hung[--nhung];
}

static void close_dialog(void)
{
    if (!dialog)
        return;
    window_close(dialog);
    dialog = NULL;
}

static int on_wait(struct widget *w, void *args, void *arg)
{
    struct hung *h = find_hung(dialog_number);
    if (h) {
        h->wait_until = uptime_ms() + WAIT_MS;
        say("client %u: wait", h->number);
    }
    close_dialog();
    show_next();
    return 1;
}

static int on_force_quit(struct widget *w, void *args, void *arg)
{
    struct hung *h = find_hung(dialog_number);
    if (h) {
        say("client %u force quit%s", h->number, h->pid ? "" : " (pid unknown, connection dropped)");
        if (h->pid > 0)
            kill((pid_t)h->pid, SIGKILL);
        session_manager_drop_client(manager, h->number);
        remove_hung(h->number);
    }
    close_dialog();
    show_next();
    return 1;
}

static void wait_ended(void *arg)
{
    (void)arg;
    wait_timer = NULL;
    show_next();
}

/* Show the dialog of the first unresponsive client that is not waited
 * for, and arm the timer for the end of the earliest wait. */
static void show_next(void)
{
    if (dialog)
        return;
    long now = uptime_ms(), next_wait = -1;
    struct hung *h = NULL;
    for (int i = 0; i < nhung; i++) {
        if (hung[i].wait_until > now) {
            if (next_wait < 0 || hung[i].wait_until < next_wait)
                next_wait = hung[i].wait_until;
        } else if (!h) {
            h = &hung[i];
        }
    }
    if (wait_timer) {
        app_timer_remove(app, wait_timer);
        wait_timer = NULL;
    }
    if (!h) {
        if (next_wait >= 0)
            wait_timer = app_timer_add(app, (int)(next_wait - now), 0, wait_ended, NULL);
        return;
    }
    h->wait_until = 0;
    char text[128];
    snprintf(text, sizeof text, _("%s is not responding."), h->title[0] ? h->title : _("The window"));
    const struct theme *t = app_theme(app);
    int width = gfx_text_width_font_scaled(t->font, text, -1, 1) + 40;
    if (width < 320)
        width = 320;
    dialog = app_layer_window(app, width, 3 * theme_px(t, TM_CONTROL_H) + 30, 2, 0, 0, 1, "sessiond");
    if (!dialog)
        return;
    dialog_number = h->number;
    label_new(dialog, text);
    struct widget *row = box_new(dialog, 0);
    struct widget *wait = button_new(row, _("Wait"));
    struct widget *quit = button_new(row, _("Force quit"));
    widget_set_stretch(wait, 1, 0);
    widget_set_stretch(quit, 1, 0);
    widget_connect(wait, "clicked", on_wait, NULL);
    widget_connect(quit, "clicked", on_force_quit, NULL);
    widget_focus(wait);
    say("dialog of client %u shown", h->number);
}

/* The creation of a window runs a roundtrip, and a roundtrip inside the
 * dispatch of an event of X12 would read the connection again in the
 * middle of the dispatch. The events therefore show the dialog from a
 * timer, outside the dispatch. */
static void show_from_timer(void *arg)
{
    (void)arg;
    show_timer = NULL;
    show_next();
}

static void schedule_show(void)
{
    if (!show_timer)
        show_timer = app_timer_add(app, 0, 0, show_from_timer, NULL);
}

/* ---- events of X12 ---- */

static void on_user(void *data, struct wire_proxy *p, int32_t uid)
{
    session_uid = uid;
    say("session uid %d", uid);
}

static void on_shortcut(void *data, struct wire_proxy *p, const char *name)
{
    if (strcmp(name, "lock") == 0)
        start_locker("Super+L");
    else if (strcmp(name, "screenshot-interface") == 0)
        start_screenshot("-i");
    else if (strcmp(name, "screenshot-screen") == 0)
        start_screenshot("-t");
    else if (strcmp(name, "screenshot-area") == 0)
        start_screenshot("-a");
}

static void on_idle(void *data, struct wire_proxy *p)
{
    start_locker("idle");
}

static void on_locker_needed(void *data, struct wire_proxy *p)
{
    start_locker("key press");
}

static void on_client_state(void *data, struct wire_proxy *p, uint32_t number, uint32_t pid, uint32_t responding,
                            const char *title)
{
    if (responding) {
        remove_hung(number);
        if (dialog && dialog_number == number) {
            close_dialog();
            say("dialog of client %u closed", number);
        }
        schedule_show();
        return;
    }
    if (find_hung(number) || nhung == MAX_HUNG)
        return;
    struct hung *h = &hung[nhung++];
    *h = (struct hung){ .number = number, .pid = pid };
    strlcpy(h->title, title, sizeof h->title);
    schedule_show();
}

static const struct session_manager_listener manager_events = { on_user, on_shortcut, on_idle, on_locker_needed,
                                                                 on_client_state };

/* The keys of the shortcuts. Print Screen opens the capture interface and
 * Shift+Print Screen saves the screen at once, as in GNOME. Super+Shift+3,
 * 4 and 5 save the screen, select an area and open the interface, as on
 * macOS (docs/design/images.md). */
static const struct {
    uint32_t key, mods;
    const char *name;
} bindings[] = {
    { KEY_L, KEYMAP_MOD_LOGO, "lock" },
    { KEY_SYSRQ, 0, "screenshot-interface" },
    { KEY_SYSRQ, KEYMAP_MOD_SHIFT, "screenshot-screen" },
    { KEY_3, KEYMAP_MOD_LOGO | KEYMAP_MOD_SHIFT, "screenshot-screen" },
    { KEY_4, KEYMAP_MOD_LOGO | KEYMAP_MOD_SHIFT, "screenshot-area" },
    { KEY_5, KEYMAP_MOD_LOGO | KEYMAP_MOD_SHIFT, "screenshot-interface" },
};

int main(void)
{
    signal(SIGPIPE, SIG_IGN);
    app = app_create();
    if (!app) {
        fprintf(stderr, "sessiond: no display\n");
        return 1;
    }
    textdomain("sessiond");
    manager = gui_bind_global("session_manager", &session_manager_interface, 1);
    if (!manager) {
        fprintf(stderr, "sessiond: X12 has no session manager interface\n");
        app_destroy(app);
        return 1;
    }
    session_manager_add_listener(manager, &manager_events, NULL);
    for (size_t i = 0; i < sizeof bindings / sizeof bindings[0]; i++)
        session_manager_bind_shortcut(manager, bindings[i].key, bindings[i].mods, bindings[i].name);
    gui_flush();
    app_timer_add(app, 1000, 1, reap, NULL);
    say("started");
    /* app_step returns 0 while sessiond has no window. The loop ends with
     * the connection. */
    while (!gui_connection_lost())
        app_step(app, -1);
    say("the display server ended");
    app_destroy(app);
    return 0;
}
