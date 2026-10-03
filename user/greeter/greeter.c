/* greeter: the graphical login (docs/design/users.md).
 *
 *     greeter [-s]
 *     greeter --window
 *
 * init runs the greeter as root on the console in place of login. It
 * starts X12 and keeps it running, and in a loop runs the login window as
 * a child, "greeter --window", which shows the accounts, checks the
 * password and reports the result on its standard output: "login NAME",
 * "poweroff" or "reboot". For a login the greeter admits the account to
 * X12 (the setting session_uid), tells init the session user, and runs
 * "startgui -s" as the account in a process group of its own. When the
 * session ends with the panel's Log out, the greeter ends the rest of the
 * group, withdraws the admission and shows the window again. -s mirrors
 * the log of X12 to the console, as the boot tests read it. */
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
#include <gui/app.h>
#include <gui/i18n.h>
#include <minios/account.h>
#include <minios/conf.h>

#define MAX_ACCOUNTS 32
#define FAILURE_DELAY_MS 1500

/* ---- the login window ---- */

static struct app *app;
static struct widget *main_win, *accounts, *password, *message, *login_button;
/* The window that asks an account without a password for a new one. */
static struct widget *choose_win, *new_field, *repeat_field, *choose_message;
static char choose_name[33];
static int choose_done;                 /* 1 cancelled, 2 password stored */
static char names[MAX_ACCOUNTS][33];
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
        char line[128];
        if (pw->pw_gecos[0])
            snprintf(line, sizeof line, "%s (%s)", pw->pw_gecos, pw->pw_name);
        else
            snprintf(line, sizeof line, "%s", pw->pw_name);
        listview_add(accounts, line);
        naccounts++;
    }
    endpwent();
}

static void report(const char *line)
{
    printf("%s\n", line);
    fflush(stdout);
    app_quit(app, 0);
}

static void retry(void *arg)
{
    waiting = 0;
    widget_set_enabled(login_button, 1);
    widget_set_text(message, "");
    widget_focus(password);
}

static int on_choose_close(struct widget *w, void *args, void *arg)
{
    choose_done = 1;
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
    int r = account_set_password(choose_name, first);
    widget_set_text(new_field, "");
    widget_set_text(repeat_field, "");
    if (r < 0) {
        widget_set_text(choose_message, _("The password cannot be stored"));
        return 1;
    }
    choose_done = 2;
    return 1;
}

static int on_new_activate(struct widget *w, void *args, void *arg)
{
    widget_focus(repeat_field);
    return 1;
}

/* An account without a password, such as root and user on a new system,
 * chooses one before its first session starts. */
static void choose_password(const char *name)
{
    snprintf(choose_name, sizeof choose_name, "%s", name);
    choose_win = app_modal_window(app, main_win, 340, 230, _("Choose a password"));
    if (!choose_win)
        return;
    char text[128];
    snprintf(text, sizeof text, _("The account %s has no password. Choose one to log in."), name);
    label_new(choose_win, text);
    label_new(choose_win, _("New password"));
    new_field = textfield_new(choose_win, "");
    textfield_set_masked(new_field, 1);
    widget_connect(new_field, "activate", on_new_activate, NULL);
    label_new(choose_win, _("Repeat the password"));
    repeat_field = textfield_new(choose_win, "");
    textfield_set_masked(repeat_field, 1);
    widget_connect(repeat_field, "activate", on_choose, NULL);
    choose_message = label_new(choose_win, "");
    struct widget *row = box_new(choose_win, 0);
    struct widget *gap = label_new(row, "");
    widget_set_stretch(gap, 1, 0);
    struct widget *cancel = button_new(row, _("Cancel"));
    widget_connect(cancel, "clicked", on_choose_close, NULL);
    struct widget *set = button_new(row, _("Set password"));
    widget_connect(set, "clicked", on_choose, NULL);
    widget_connect(choose_win, "close", on_choose_close, NULL);
    widget_focus(new_field);
    choose_done = 0;
    while (!choose_done && app_step(app, -1))
        ;
    window_close(choose_win);
    app_step(app, 0);
    choose_win = NULL;
    if (choose_done == 2) {
        char line[64];
        snprintf(line, sizeof line, "login %s", choose_name);
        report(line);
    } else {
        widget_focus(password);
    }
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
        choose_password(name);
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

static int on_select(struct widget *w, void *args, void *arg)
{
    selected = ((struct sig_select *)args)->index;
    widget_focus(password);
    return 1;
}

static int on_power(struct widget *w, void *args, void *arg)
{
    report(arg);
    return 1;
}

static int window_main(void)
{
    app = app_create();
    if (!app)
        return 1;
    textdomain("greeter");
    struct widget *win = main_win = app_window(app, 360, 320, _("Log in"));
    if (!win)
        return 1;
    struct utsname u;
    const char *host = uname(&u) == 0 && u.nodename[0] ? u.nodename : "minios";
    char welcome[128];
    snprintf(welcome, sizeof welcome, _("Welcome to %s"), host);
    label_new(win, welcome);
    label_new(win, _("Account"));
    accounts = listview_new(win);
    widget_set_stretch(accounts, 1, 1);
    widget_connect(accounts, "selected", on_select, NULL);
    widget_connect(accounts, "activate", on_select, NULL);
    load_accounts();
    /* The first account after root is the likely one. */
    selected = naccounts > 1 ? 1 : 0;
    widget_set_value(accounts, selected);
    label_new(win, _("Password"));
    password = textfield_new(win, "");
    textfield_set_masked(password, 1);
    widget_connect(password, "activate", on_login, NULL);
    message = label_new(win, "");
    struct widget *row = box_new(win, 0);
    struct widget *restart = button_new(row, _("Restart"));
    widget_connect(restart, "clicked", on_power, "reboot");
    struct widget *off = button_new(row, _("Shut down"));
    widget_connect(off, "clicked", on_power, "poweroff");
    struct widget *gap = label_new(row, "");
    widget_set_stretch(gap, 1, 0);
    login_button = button_new(row, _("Log in"));
    widget_connect(login_button, "clicked", on_login, NULL);
    widget_focus(password);
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
static void start_server(void)
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
            return;
        sleep_ms(100);
    }
    fprintf(stderr, "greeter: the display server does not answer\n");
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
        if (initgroups(name, gid) < 0 || setgid(gid) < 0 || setuid(uid) < 0)
            _exit(126);
        if (chdir(home) < 0)
            chdir("/");
        static char e_home[300], e_user[64], e_logname[64], e_shell[300];
        snprintf(e_home, sizeof e_home, "HOME=%s", home);
        snprintf(e_user, sizeof e_user, "USER=%s", name);
        snprintf(e_logname, sizeof e_logname, "LOGNAME=%s", name);
        snprintf(e_shell, sizeof e_shell, "SHELL=%s", shell);
        char *envp[] = { e_home, e_user, e_logname, e_shell, "TERM=minios", "PATH=/bin:/usr/local/bin", NULL };
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
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    struct sockaddr_un addr = { AF_UNIX, "init" };
    char line[32];
    snprintf(line, sizeof line, "%s\n", request);
    if (fd >= 0 && connect(fd, (struct sockaddr *)&addr, sizeof addr) == 0)
        write(fd, line, strlen(line));
    if (fd >= 0)
        close(fd);
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--window") == 0)
        return window_main();
    server_log_serial = argc == 2 && strcmp(argv[1], "-s") == 0;
    if (geteuid() != 0) {
        fprintf(stderr, "greeter: must be run by root\n");
        return 1;
    }
    conf_export_locale();
    start_server();
    for (;;) {
        char line[96];
        if (ask(line, sizeof line) < 0) {
            sleep_ms(500);
            continue;
        }
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
