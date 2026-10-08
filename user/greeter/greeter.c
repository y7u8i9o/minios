/* greeter: the graphical login (docs/design/users.md).
 *
 *     greeter [-s] [-a NAME]
 *     greeter --window
 *
 * init runs the greeter as root on the console in place of login. It
 * starts X12 and ensures that it continues to run, and in a loop runs the login window as
 * a child, "greeter --window", which shows the accounts, checks the
 * password and reports the result on its standard output: "login NAME",
 * "poweroff" or "reboot". For a login the greeter admits the account to
 * X12 (the setting session_uid), tells init the session user, and runs
 * "startgui -s" as the account in a process group of its own. When the
 * session ends with the panel's Log out, the greeter ends the rest of the
 * group, withdraws the admission and shows the window again. -s mirrors
 * the log of X12 to the console, as the boot tests read it. -a NAME starts
 * the session of the account NAME once without the login window, which
 * the live medium uses for its account live (docs/design/live.md). The
 * login window follows when that session ends.
 *
 * Without a display, or when X12 or the login window cannot start, the
 * greeter runs the console login in its place, which lets the default
 * init.conf serve machines without a framebuffer as well. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <pwd.h>
#include <grp.h>
#include <shadow.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/utsname.h>
#include <time.h>
#include <gui/app.h>
#include <gui/theme.h>
#include <minios/input.h>
#include <minios/init.h>
#include <gui/i18n.h>
#include <minios/account.h>
#include <minios/conf.h>
#include "screen.h"

#define MAX_ACCOUNTS 32
#define FAILURE_DELAY_MS 1500

/* ---- the login window ---- */

/* The login window is a layer surface over the whole screen, laid out in
 * the manner of GDM (screen.h). A top bar shows the host name, the clock
 * and the power buttons. A card in the middle shows one of three pages:
 * the accounts, the password of the chosen account, and the first
 * password of an account that has none. */

enum page { PAGE_USERS, PAGE_PASSWORD, PAGE_CHOOSE };

static struct app *app;
static struct widget *main_win, *card;
static struct widget *pages[3];
static struct widget *rows[MAX_ACCOUNTS];
static struct widget *pw_header, *password, *message, *login_button;
static struct widget *ch_header, *new_field, *repeat_field, *choose_message;
static char names[MAX_ACCOUNTS][33];
static char full_names[MAX_ACCOUNTS][64];
static int naccounts, selected;
static int waiting;                     /* after a wrong password */

static void load_accounts(void)
{
    setpwent();
    struct passwd *pw;
    while ((pw = getpwent()) && naccounts < MAX_ACCOUNTS) {
        /* root and the accounts of people, not system accounts. */
        if (pw->pw_uid != 0 && pw->pw_uid < ACCOUNT_FIRST_ID)
            continue;
        snprintf(names[naccounts], sizeof names[0], "%s", pw->pw_name);
        snprintf(full_names[naccounts], sizeof full_names[0], "%s", pw->pw_gecos[0] ? pw->pw_gecos : pw->pw_name);
        naccounts++;
    }
    endpwent();
}

/* ---- the pages ---- */

static void report(const char *line)
{
    printf("%s\n", line);
    fflush(stdout);
    app_quit(app, 0);
}

static void show_page(enum page page)
{
    for (int i = 0; i < 3; i++)
        widget_set_visible(pages[i], i == (int)page);
    widget_relayout(main_win);
    if (page == PAGE_USERS)
        widget_focus(rows[selected]);
    else if (page == PAGE_PASSWORD)
        widget_focus(password);
    else
        widget_focus(new_field);
}

static void retry(void *arg)
{
    waiting = 0;
    widget_set_enabled(login_button, 1);
    widget_set_text(message, "");
    widget_focus(password);
}

static int on_account(struct widget *w, void *args, void *arg)
{
    for (int i = 0; i < naccounts; i++)
        if (rows[i] == w)
            selected = i;
    account_set(pw_header, names[selected], full_names[selected]);
    account_set(ch_header, names[selected], full_names[selected]);
    widget_set_text(password, "");
    widget_set_text(message, "");
    show_page(PAGE_PASSWORD);
    return 1;
}

static int on_back(struct widget *w, void *args, void *arg)
{
    widget_set_text(password, "");
    widget_set_text(new_field, "");
    widget_set_text(repeat_field, "");
    widget_set_text(choose_message, "");
    show_page(PAGE_USERS);
    return 1;
}

static int on_login(struct widget *w, void *args, void *arg)
{
    if (waiting || selected < 0 || selected >= naccounts)
        return 1;
    const char *name = names[selected];
    struct spwd *sp = getspnam(name);
    int ok = sp && account_check(widget_text(password), sp->sp_pwdp);
    widget_set_text(password, "");
    if (ok && !sp->sp_pwdp[0]) {
        /* An account without a password, such as root and user on a new
         * system, chooses one before its first session starts. */
        show_page(PAGE_CHOOSE);
        return 1;
    }
    if (ok) {
        char line[64];
        snprintf(line, sizeof line, "login %s", name);
        report(line);
        return 1;
    }
    /* A short pause after each wrong password slows down guessing. */
    widget_set_text(message, _("Wrong password"));
    widget_set_enabled(login_button, 0);
    waiting = 1;
    app_timer_add(app, FAILURE_DELAY_MS, 0, retry, NULL);
    return 1;
}

static int on_new_activate(struct widget *w, void *args, void *arg)
{
    widget_focus(repeat_field);
    return 1;
}

static int on_choose(struct widget *w, void *args, void *arg)
{
    const char *first = widget_text(new_field), *second = widget_text(repeat_field);
    if (!first[0]) {
        widget_set_text(choose_message, _("The password must not be empty"));
        return 1;
    }
    if (strcmp(first, second) != 0) {
        widget_set_text(choose_message, _("The passwords differ"));
        widget_set_text(repeat_field, "");
        widget_focus(repeat_field);
        return 1;
    }
    int r = account_set_password(names[selected], first);
    widget_set_text(new_field, "");
    widget_set_text(repeat_field, "");
    if (r < 0) {
        widget_set_text(choose_message, _("The password cannot be stored"));
        return 1;
    }
    char line[64];
    snprintf(line, sizeof line, "login %s", names[selected]);
    report(line);
    return 1;
}

static int on_power(struct widget *w, void *args, void *arg)
{
    report(arg);
    return 1;
}

static struct widget *masked_field(struct widget *parent, signal_fn activate)
{
    struct widget *f = textfield_new(parent, "");
    textfield_set_masked(f, 1);
    widget_connect(f, "activate", activate, NULL);
    return f;
}

static struct widget *button_row(struct widget *parent, const char *back, const char *forward, signal_fn fn,
                                 struct widget **forward_button)
{
    struct widget *row = box_new(parent, 0);
    struct widget *b = button_new(row, back);
    widget_connect(b, "clicked", on_back, NULL);
    struct widget *gap = label_new(row, "");
    widget_set_stretch(gap, 1, 0);
    struct widget *f = button_new(row, forward);
    widget_connect(f, "clicked", fn, NULL);
    if (forward_button)
        *forward_button = f;
    return row;
}

static int window_main(void)
{
    app = app_create();
    if (!app)
        return 1;
    textdomain("greeter");
    load_accounts();
    if (naccounts == 0)
        return 1;
    struct widget *win = main_win =
        app_layer_window(app, 0, 0, 3, GUI_ANCHOR_TOP | GUI_ANCHOR_BOTTOM | GUI_ANCHOR_LEFT | GUI_ANCHOR_RIGHT,
                         0, 1, "greeter");
    if (!win)
        return 1;
    struct utsname u;
    const char *host = uname(&u) == 0 && u.nodename[0] ? u.nodename : "minios";

    /* The top bar: the host name, the clock in the middle, the power
     * buttons on the right. */
    struct widget *back = backdrop_new(win, BACKDROP_DESKTOP);
    struct widget *right = screen_bar_new(back, host);
    struct widget *restart = button_new(right, _("Restart"));
    widget_connect(restart, "clicked", on_power, "reboot");
    struct widget *off = button_new(right, _("Shut down"));
    widget_connect(off, "clicked", on_power, "poweroff");
    card = screen_card_new(back);

    /* The accounts. */
    pages[PAGE_USERS] = box_new(card, 1);
    char welcome[128];
    snprintf(welcome, sizeof welcome, _("Welcome to %s"), host);
    label_new(pages[PAGE_USERS], welcome);
    for (int i = 0; i < naccounts; i++) {
        rows[i] = account_new(pages[PAGE_USERS], names[i], full_names[i], 1);
        widget_connect(rows[i], "clicked", on_account, NULL);
    }

    /* The password of the chosen account. */
    pages[PAGE_PASSWORD] = box_new(card, 1);
    pw_header = account_new(pages[PAGE_PASSWORD], names[0], full_names[0], 0);
    label_new(pages[PAGE_PASSWORD], _("Password"));
    password = masked_field(pages[PAGE_PASSWORD], on_login);
    message = label_new(pages[PAGE_PASSWORD], "");
    button_row(pages[PAGE_PASSWORD], _("Back"), _("Log in"), on_login, &login_button);

    /* The first password of an account that has none. */
    pages[PAGE_CHOOSE] = box_new(card, 1);
    ch_header = account_new(pages[PAGE_CHOOSE], names[0], full_names[0], 0);
    label_new(pages[PAGE_CHOOSE], _("This account has no password yet."));
    label_new(pages[PAGE_CHOOSE], _("Choose one to log in."));
    label_new(pages[PAGE_CHOOSE], _("New password"));
    new_field = masked_field(pages[PAGE_CHOOSE], on_new_activate);
    label_new(pages[PAGE_CHOOSE], _("Repeat the password"));
    repeat_field = masked_field(pages[PAGE_CHOOSE], on_choose);
    choose_message = label_new(pages[PAGE_CHOOSE], "");
    button_row(pages[PAGE_CHOOSE], _("Back"), _("Set password"), on_choose, NULL);

    /* The first account after root is the likely one. */
    selected = naccounts > 1 ? 1 : 0;
    screen_clock_start(app);
    show_page(PAGE_USERS);
    int r = app_run(app);
    app_destroy(app);
    return r;
}

/* ---- the supervisor ---- */

static pid_t server = -1;
static int server_log_serial;

static pid_t spawn(char *const argv[], int out_fd)
{
    pid_t pid = fork();
    if (pid == 0) {
        if (out_fd >= 0) {
            dup2(out_fd, 1);
            close(out_fd);
        }
        execvp(argv[0], argv);
        _exit(127);
    }
    return pid;
}

/* Start X12 and wait until it accepts connections. */
static int wait_child(pid_t pid);

/* Replace the greeter with the console login. */
static void fall_back(const char *reason)
{
    fprintf(stderr, "greeter: %s, starting the console login\n", reason);
    if (server > 0) {
        kill(server, SIGTERM);
        wait_child(server);
    }
    char *argv[] = { "login", NULL };
    execv("/bin/login", argv);
    perror("greeter: /bin/login");
    exit(1);
}

/* Start X12 and wait until it answers. Returns 0, or -1 when it does not
 * answer within five seconds. */
static int start_server(void)
{
    char *argv[] = { "x12", server_log_serial ? "-s" : NULL, NULL };
    server = spawn(argv, -1);
    for (int i = 0; i < 50; i++) {
        int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        struct sockaddr_un addr = { AF_UNIX, "display" };
        int ok = fd >= 0 && connect(fd, (struct sockaddr *)&addr, sizeof addr) == 0;
        if (fd >= 0)
            close(fd);
        if (ok)
            return 0;
        sleep_ms(100);
    }
    return -1;
}

/* Wait for pid, restarting X12 when it is the one that ended. Returns the
 * status of pid, or -1 when X12 ended first. */
static int wait_child(pid_t pid)
{
    for (;;) {
        int status;
        pid_t done = waitpid(-1, &status, 0);
        if (done < 0)
            return -1;
        if (done == server) {
            fprintf(stderr, "greeter: the display server ended with status 0x%x, restarting\n", status);
            start_server();
            if (pid != server)
                return -1;
            continue;
        }
        if (done == pid)
            return status;
    }
}

/* Run the login window and read its answer into line. */
static int ask(char *line, size_t size)
{
    int fds[2];
    if (pipe(fds) < 0)
        return -1;
    char self[] = "/bin/greeter";
    char *argv[] = { self, "--window", NULL };
    pid_t pid = spawn(argv, fds[1]);
    close(fds[1]);
    size_t got = 0;
    ssize_t n;
    while (got + 1 < size && (n = read(fds[0], line + got, size - 1 - got)) > 0)
        got += (size_t)n;
    close(fds[0]);
    line[got] = '\0';
    line[strcspn(line, "\n")] = '\0';
    wait_child(pid);
    return got ? 0 : -1;
}

static void set_session_uid(int uid)
{
    char value[16];
    snprintf(value, sizeof value, "%d", uid);
    char *argv[] = { "x12settings", "set", "session_uid", value, NULL };
    pid_t pid = spawn(argv, -1);
    if (pid > 0)
        wait_child(pid);
}

static void run_session(const char *name)
{
    struct passwd *pw = getpwnam(name);
    if (!pw)
        return;
    uid_t uid = pw->pw_uid;
    gid_t gid = pw->pw_gid;
    char home[256], shell[256];
    snprintf(home, sizeof home, "%s", pw->pw_dir[0] ? pw->pw_dir : "/");
    snprintf(shell, sizeof shell, "%s", pw->pw_shell[0] ? pw->pw_shell : "/bin/sh");
    printf("greeter: session of %s\n", name);
    fflush(stdout);
    set_session_uid((int)uid);
    account_session((int)uid);
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        if (account_become(name, uid, gid) < 0)
            _exit(126);
        if (chdir(home) < 0)
            chdir("/");
        static char e_home[300], e_user[64], e_logname[64], e_shell[300];
        snprintf(e_home, sizeof e_home, "HOME=%s", home);
        snprintf(e_user, sizeof e_user, "USER=%s", name);
        snprintf(e_logname, sizeof e_logname, "LOGNAME=%s", name);
        snprintf(e_shell, sizeof e_shell, "SHELL=%s", shell);
        char *envp[] = { e_home, e_user, e_logname, e_shell, "TERM=minios", "PATH=/usr/bin:/usr/local/bin", NULL };
        char *argv[] = { "/bin/startgui", "-s", NULL };
        execve(argv[0], argv, envp);
        _exit(127);
    }
    if (pid > 0) {
        wait_child(pid);
        /* Programs the session left behind end with it. */
        kill(-pid, SIGTERM);
        sleep_ms(300);
        kill(-pid, SIGKILL);
    }
    set_session_uid(-1);
    account_session(-1);
    printf("greeter: session of %s ended\n", name);
    fflush(stdout);
}

static void power(const char *request)
{
    init_request(request, NULL, 0);
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--window") == 0)
        return window_main();
    const char *autologin = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-s") == 0) {
            server_log_serial = 1;
        } else if (strcmp(argv[i], "-a") == 0 && i + 1 < argc) {
            autologin = argv[++i];
        } else {
            fprintf(stderr, "usage: greeter [-s] [-a NAME]\n");
            return 2;
        }
    }
    if (geteuid() != 0) {
        fprintf(stderr, "greeter: must be run by root\n");
        return 1;
    }
    conf_export_locale();
    if (access("/dev/fb0", R_OK | W_OK) < 0)
        fall_back("no display");
    if (start_server() < 0)
        fall_back("the display server does not answer");
    if (autologin && getpwnam(autologin)) {
        fprintf(stderr, "greeter: display server running, starting the session of %s\n", autologin);
        run_session(autologin);
    } else if (autologin) {
        fprintf(stderr, "greeter: there is no account %s\n", autologin);
    }
    fprintf(stderr, "greeter: display server running, showing the login window\n");
    int failures = 0;
    for (;;) {
        char line[96];
        if (ask(line, sizeof line) < 0) {
            if (++failures == 5)
                fall_back("the login window does not start");
            sleep_ms(500);
            continue;
        }
        failures = 0;
        if (strncmp(line, "login ", 6) == 0) {
            run_session(line + 6);
        } else if (strcmp(line, "poweroff") == 0 || strcmp(line, "reboot") == 0) {
            power(line);
            /* init ends the greeter with the other programs. */
            for (;;)
                sleep_ms(1000);
        }
    }
}
