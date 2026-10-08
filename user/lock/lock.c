/* lock: the screen locker (docs/design/lock.md).
 *
 *     lock
 *
 * lock locks the session of X12 and covers the screen with a window in
 * the style of the greeter (screen.h). The window shows the account of
 * the real uid and a password field. The setuid helper checkpass checks
 * the password. The right password unlocks the session, and lock exits.
 * lock exits with status 1 when X12 refuses the lock, for example while
 * another locker runs.
 *
 * X12 starts lock for Super+L and after the idle timeout. The power menu
 * of the panel starts it as well. The program writes "lock: locked",
 * "lock: wrong password" and "lock: unlocked" to its standard output for
 * the boot tests. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pwd.h>
#include <sys/utsname.h>
#include <gui/app.h>
#include <gui/i18n.h>
#include "../greeter/screen.h"

#define CHECKPASS "/bin/checkpass"

static struct app *app;
static struct widget *window, *password, *message, *unlock_button;
static int checking;

static void say(const char *line)
{
    printf("lock: %s\n", line);
    fflush(stdout);
}

static int on_unlock(struct widget *w, void *args, void *arg)
{
    if (checking)
        return 1;
    char input[260];
    snprintf(input, sizeof input, "%s\n", widget_text(password));
    widget_set_text(password, "");
    checking = 1;
    widget_set_enabled(unlock_button, 0);
    widget_set_text(message, _("Checking the password..."));
    char out[256];
    char *const argv[] = { CHECKPASS, NULL };
    int status = app_run_command(app, argv, input, out, sizeof out, NULL);
    memset(input, 0, sizeof input);
    checking = 0;
    widget_set_enabled(unlock_button, 1);
    if (status == 0) {
        if (app_unlock(window) == 0)
            say("unlocked");
        app_quit(app, 0);
        return 1;
    }
    say(status == 1 ? "wrong password" : "cannot check the password");
    widget_set_text(message, status == 1 ? _("Wrong password") : _("The password cannot be checked"));
    widget_focus(password);
    return 1;
}

int main(void)
{
    app = app_create();
    if (!app) {
        fprintf(stderr, "lock: no display\n");
        return 1;
    }
    textdomain("lock");
    struct passwd *pw = getpwuid(getuid());
    char name[64], full_name[128];
    snprintf(name, sizeof name, "%s", pw ? pw->pw_name : "?");
    snprintf(full_name, sizeof full_name, "%s", pw && pw->pw_gecos[0] ? pw->pw_gecos : name);
    window = app_lock_window(app);
    if (!window) {
        fprintf(stderr, "lock: X12 refused the lock\n");
        app_destroy(app);
        return 1;
    }
    struct utsname u;
    const char *host = uname(&u) == 0 && u.nodename[0] ? u.nodename : "minios";
    struct widget *back = backdrop_new(window, BACKDROP_DESKTOP);
    screen_bar_new(back, host);
    struct widget *card = screen_card_new(back);
    label_new(card, _("This session is locked."));
    account_new(card, name, full_name, 0);
    label_new(card, _("Password"));
    password = textfield_new(card, "");
    textfield_set_masked(password, 1);
    widget_connect(password, "activate", on_unlock, NULL);
    message = label_new(card, "");
    struct widget *row = box_new(card, 0);
    struct widget *gap = label_new(row, "");
    widget_set_stretch(gap, 1, 0);
    unlock_button = button_new(row, _("Unlock"));
    widget_connect(unlock_button, "clicked", on_unlock, NULL);
    screen_clock_start(app);
    widget_focus(password);
    say("locked");
    int r = app_run(app);
    app_destroy(app);
    return r;
}
