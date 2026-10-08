/* askpass: the authentication dialog of the desktop (docs/design/users.md).
 *
 *     askpass [PROMPT]
 *
 * sudo runs askpass for "sudo -A", as /etc/sudo.conf names it, with the
 * prompt as its argument and the credentials of the user. askpass writes
 * the password and a newline to its standard output, which is a pipe to
 * sudo. Cancel writes nothing, and askpass exits with status 1.
 *
 * The dialog is a layer surface of the overlay layer over the whole
 * screen, in the manner of GNOME. The screen is dimmed around a card in
 * the middle. The card shows the reason, the avatar and the name of the
 * user, the password field, and the buttons Cancel and Authenticate. The
 * window has ARGB buffers. Its opaque region is the card, and X12 blends
 * the dimmed rest with its alpha. The overlay has the keyboard focus, and
 * X12 refuses Alt+Tab and Alt+F4 while it has it.
 *
 * sudo runs askpass again after a wrong password. The state file of the
 * run of sudo (askpass_state_path, with the pid of the parent) tells the
 * second dialog to report the wrong password. Cancel writes "cancel" into
 * the file, which app_run_privileged reads. State files of runs of sudo
 * that have ended are removed at the start. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <signal.h>
#include <unistd.h>
#include <pwd.h>
#include <gui/app.h>
#include <gui/client.h>
#include <gui/privilege.h>
#include <gui/theme.h>
#include <gui/i18n.h>
#include <minios/input.h>

#define CARD_W 400
#define BUTTON_W 160                    /* both buttons, so that they are equal */

static struct app *app;
static struct widget *window, *field;
static char state_path[64];
static const char *user_name = "", *full_name = "";
static int result = 1;

static void finish(int code)
{
    result = code;
    app_quit(app, code);
}

static void cancel(void)
{
    int fd = open(state_path, O_WRONLY | O_TRUNC);
    if (fd >= 0) {
        write(fd, "cancel\n", 7);
        close(fd);
    }
    finish(1);
}

static int on_authenticate(struct widget *w, void *args, void *arg)
{
    const char *password = widget_text(field);
    printf("%s\n", password);
    fflush(stdout);
    widget_set_text(field, "");
    finish(0);
    return 1;
}

static int on_cancel(struct widget *w, void *args, void *arg)
{
    cancel();
    return 1;
}

/* ---- the state of the run of sudo ---- */

/* Remove the state files of this user whose run of sudo has ended. */
static void remove_stale(uid_t uid)
{
    DIR *d = opendir("/tmp");
    if (!d)
        return;
    char prefix[32];
    int n = snprintf(prefix, sizeof prefix, ".askpass-%d-", (int)uid);
    struct dirent *e;
    while ((e = readdir(d))) {
        if (strncmp(e->d_name, prefix, (size_t)n) != 0)
            continue;
        pid_t pid = (pid_t)atoi(e->d_name + n);
        if (pid > 0 && kill(pid, 0) < 0 && errno == ESRCH) {
            char path[64];
            askpass_state_path(path, sizeof path, uid, pid);
            unlink(path);
        }
    }
    closedir(d);
}

/* Returns 1 when an earlier dialog of the same run of sudo was answered,
 * which means that its password was wrong. */
static int open_state(void)
{
    uid_t uid = getuid();
    remove_stale(uid);
    askpass_state_path(state_path, sizeof state_path, uid, getppid());
    int fd = open(state_path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd >= 0) {
        close(fd);
        return 0;
    }
    return errno == EEXIST;
}

/* sudo's own prompt, "[sudo] password for NAME: ", or an empty one, gives
 * no reason. */
static const char *reason_of(const char *prompt)
{
    if (!prompt[0] || strncmp(prompt, "[sudo]", 6) == 0 || strncmp(prompt, "Password", 8) == 0)
        return _("Authentication is required to run a command as root.");
    return prompt;
}

int main(int argc, char **argv)
{
    const char *prompt = argc > 1 ? argv[1] : "";
    int retry = open_state();
    struct passwd *pw = getpwuid(getuid());
    if (pw) {
        user_name = strdup(pw->pw_name);
        full_name = strdup(pw->pw_gecos[0] ? pw->pw_gecos : pw->pw_name);
    }
    app = app_create();
    if (!app) {
        fprintf(stderr, "askpass: cannot connect to the display server\n");
        return 1;
    }
    textdomain("askpass");
    /* The size of the screen at the top left corner covers the panel as
     * well. A size of 0 would leave out the area of the panel. */
    window = app_layer_window(app, gui_screen_width(), gui_screen_height(), 3, GUI_ANCHOR_TOP | GUI_ANCHOR_LEFT, 0,
                              1, "askpass");
    if (!window) {
        fprintf(stderr, "askpass: cannot open the window\n");
        return 1;
    }
    /* The backdrop dims the screen. The card is the opaque region of the
     * window. Escape in the card cancels. */
    window_set_translucent(window);
    struct widget *back = backdrop_new(window, BACKDROP_DIM);
    spacer_new(back);
    struct widget *card = card_new(back, CARD_W);
    widget_connect(card, "cancel", on_cancel, NULL);
    label_new(card, _("Authentication required"));
    label_new(card, reason_of(prompt));
    account_new(card, user_name, full_name, 0);
    field = textfield_new(card, "");
    textfield_set_masked(field, 1);
    widget_connect(field, "activate", on_authenticate, NULL);
    if (retry)
        label_new(card, _("The password was wrong. Try again."));
    struct widget *row = box_new(card, 0);
    struct widget *cancel_button = button_new(row, _("Cancel"));
    widget_set_stretch(cancel_button, 1, 0);
    widget_set_min(cancel_button, BUTTON_W, 0);
    widget_connect(cancel_button, "clicked", on_cancel, NULL);
    struct widget *ok = button_new(row, _("Authenticate"));
    widget_set_stretch(ok, 1, 0);
    widget_set_min(ok, BUTTON_W, 0);
    widget_connect(ok, "clicked", on_authenticate, NULL);

    spacer_new(back);
    widget_focus(field);
    widget_connect(window, "close", on_cancel, NULL);
    app_run(app);
    app_destroy(app);
    return result;
}
