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
 * the log of X12 to the console, as the boot tests read it.
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
#include <gui/image.h>
#include <gui/theme.h>
#include <minios/input.h>
#include <gui/i18n.h>
#include <minios/account.h>
#include <minios/conf.h>

#define MAX_ACCOUNTS 32
#define FAILURE_DELAY_MS 1500

/* ---- the login window ---- */

/* The login window is a layer surface over the whole screen, laid out in
 * the manner of GDM. A top bar shows the host name, the clock and the
 * power buttons. A card in the middle shows one of three pages: the
 * accounts, the password of the chosen account, and the first password of
 * an account that has none. The background is the desktop colour of
 * /etc/desktop.conf with a gradient, or its wallpaper when one is set. */

#define CARD_W 340
#define AVATAR 40
#define ROW_H  52

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
static uint32_t desktop_color = 0x00306080;
static struct image *wallpaper;

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

/* The desktop colour and the wallpaper of the system's desktop settings. */
static void load_background(void)
{
    FILE *f = fopen("/etc/desktop.conf", "r");
    if (!f)
        return;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\n")] = '\0';
        if (strncmp(line, "desktop_color=", 14) == 0)
            desktop_color = (uint32_t)strtoul(line + 14, NULL, 0) & 0xffffff;
        else if (strncmp(line, "wallpaper=", 10) == 0 && line[10])
            wallpaper = image_load(line + 10);
    }
    fclose(f);
}

/* color with each channel scaled by percent. */
static uint32_t shade(uint32_t color, int percent)
{
    uint32_t r = ((color >> 16) & 0xff) * (uint32_t)percent / 100;
    uint32_t g = ((color >> 8) & 0xff) * (uint32_t)percent / 100;
    uint32_t b = (color & 0xff) * (uint32_t)percent / 100;
    return (r > 255 ? 255 : r) << 16 | (g > 255 ? 255 : g) << 8 | (b > 255 ? 255 : b);
}

/* ---- the widgets of the window ---- */

/* The backdrop is a vertical box that paints the background. */
static void backdrop_measure(struct widget *w, struct size_hint *h) { box_class.measure(w, h); }
static void backdrop_layout(struct widget *w) { box_class.layout(w); }

static void backdrop_paint(struct widget *w, struct painter *p)
{
    if (wallpaper) {
        /* The wallpaper covers the screen and is cut at the longer side. */
        int iw = wallpaper->w / wallpaper->scale, ih = wallpaper->h / wallpaper->scale;
        int sw = w->w, sh = ih * w->w / (iw ? iw : 1);
        if (sh < w->h) {
            sh = w->h;
            sw = iw * w->h / (ih ? ih : 1);
        }
        painter_image_scaled(p, (w->w - sw) / 2, (w->h - sh) / 2, sw, sh, wallpaper);
        return;
    }
    for (int y = 0; y < w->h; y += 4)
        painter_fill(p, 0, y, w->w, 4, shade(desktop_color, 115 - 55 * y / (w->h ? w->h : 1)));
}

static const struct widget_class backdrop_class = { "greeter-backdrop", sizeof(struct widget), backdrop_measure,
                                                    backdrop_layout, backdrop_paint, NULL, NULL };

/* The top bar and the card are boxes with a background of the theme. */
static void bar_paint(struct widget *w, struct painter *p)
{
    const struct theme *t = widget_theme(w);
    painter_fill(p, 0, 0, w->w, w->h, t->color[TC_WINDOW]);
    painter_fill(p, 0, w->h - 1, w->w, 1, t->color[TC_BORDER]);
}

static void card_paint(struct widget *w, struct painter *p)
{
    const struct theme *t = widget_theme(w);
    painter_rounded(p, 0, 0, w->w, w->h, t->color[TC_WINDOW], t->color[TC_BORDER]);
}

/* The clock is centred on the whole bar, whatever the widths of the host
 * name on its left and of the buttons on its right. */
static struct widget *clock_label;

static void bar_layout(struct widget *w)
{
    box_class.layout(w);
    if (clock_label && clock_label->parent == w)
        clock_label->x = (w->w - clock_label->w) / 2;
}

static const struct widget_class bar_class = { "greeter-bar", sizeof(struct widget), backdrop_measure,
                                               bar_layout, bar_paint, NULL, NULL };
/* A box that paints nothing, for the buttons beside the centred clock. */
static const struct widget_class clear_box_class = { "greeter-box", sizeof(struct widget), backdrop_measure,
                                                     backdrop_layout, NULL, NULL, NULL };
static const struct widget_class card_class = { "greeter-card", sizeof(struct widget), backdrop_measure,
                                                backdrop_layout, card_paint, NULL, NULL };

/* A spacer takes the free space of the backdrop and paints nothing. */
static void spacer_measure(struct widget *w, struct size_hint *h) { (void)w; (void)h; }

static const struct widget_class spacer_class = { "greeter-spacer", sizeof(struct widget), spacer_measure, NULL,
                                                  NULL, NULL, NULL };

static struct widget *container_new(const struct widget_class *cls, struct widget *parent, int vertical, int padding)
{
    struct widget *w = widget_new(cls, parent);
    if (w) {
        w->value = vertical;
        w->padding = padding;
    }
    return w;
}

/* An account row shows an avatar with the initial of the account, the
 * full name and the account name. value is the index of the account. In
 * the list a row is focusable and emits "clicked" for a click, Enter or
 * Space, and the arrow keys move between the rows. Above the password the
 * same widget is a header that does not take the focus. */
static uint32_t avatar_color(const char *name)
{
    static const uint32_t colors[] = { 0x003c78c8, 0x00c0504d, 0x009bbb59, 0x008064a2, 0x00f79646, 0x004bacc6 };
    unsigned h = 0;
    for (const char *s = name; *s; s++)
        h = h * 31 + (unsigned char)*s;
    return colors[h % (sizeof colors / sizeof colors[0])];
}

static void row_measure(struct widget *w, struct size_hint *h)
{
    h->min_h = h->pref_h = ROW_H;
    h->min_w = h->pref_w = CARD_W - 40;
}

static void row_paint(struct widget *w, struct painter *p)
{
    const struct theme *t = widget_theme(w);
    int i = w->value, active = w->focusable && (w->focused || w->hover);
    if (i < 0 || i >= naccounts)
        return;
    uint32_t text = t->color[TC_TEXT], dim = t->color[TC_TEXT_DISABLED];
    if (w->focusable && w->focused) {
        painter_rounded(p, 0, 0, w->w, w->h, t->color[TC_SELECTION], t->color[TC_SELECTION]);
        text = dim = t->color[TC_SELECTION_TEXT];
    } else if (active) {
        painter_rounded(p, 0, 0, w->w, w->h, t->color[TC_BUTTON_HOVER], t->color[TC_BUTTON_HOVER]);
    }
    int ay = (w->h - AVATAR) / 2;
    uint32_t c = avatar_color(names[i]);
    painter_rounded(p, 8, ay, AVATAR, AVATAR, c, c);
    char initial[2] = { full_names[i][0], '\0' };
    if (initial[0] >= 'a' && initial[0] <= 'z')
        initial[0] = (char)(initial[0] - 'a' + 'A');
    int fh = t->metric[TM_FONT_PX];
    painter_text(p, 8 + (AVATAR - painter_text_width(p, initial, 1)) / 2, ay + (AVATAR - fh) / 2 - 1, initial,
                 0x00ffffff);
    int tx = 8 + AVATAR + 12, ty = (w->h - 2 * fh - 4) / 2;
    painter_text(p, tx, ty, full_names[i], text);
    painter_text(p, tx, ty + fh + 4, names[i], dim);
    if (w->focused)
        painter_focus_ring(p, 0, 0, w->w, w->h);
}

static int row_event(struct widget *w, struct event *e)
{
    if (!w->focusable)
        return 0;
    if (e->type == EV_MOUSE_UP && e->x >= 0 && e->y >= 0 && e->x < w->w && e->y < w->h) {
        widget_emit(w, "clicked", NULL);
        return 1;
    }
    if (e->type == EV_KEY_DOWN && (e->code == KEY_ENTER || e->code == KEY_SPACE)) {
        widget_emit(w, "clicked", NULL);
        return 1;
    }
    if (e->type == EV_KEY_DOWN && (e->code == KEY_UP || e->code == KEY_DOWN)) {
        int i = w->value + (e->code == KEY_DOWN ? 1 : -1);
        if (i >= 0 && i < naccounts)
            widget_focus(rows[i]);
        return 1;
    }
    if (e->type == EV_ENTER || e->type == EV_LEAVE || e->type == EV_FOCUS_IN || e->type == EV_FOCUS_OUT)
        widget_invalidate(w);
    return 0;
}

static const struct widget_class row_class = { "greeter-account", sizeof(struct widget), row_measure, NULL,
                                               row_paint, row_event, NULL };

static struct widget *row_new(struct widget *parent, int index, int focusable)
{
    struct widget *w = widget_new(&row_class, parent);
    if (w) {
        w->value = index;
        w->focusable = focusable ? 1 : 0;
    }
    return w;
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

static void tick(void *arg)
{
    char text[64];
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    strftime(text, sizeof text, "%a %d %b  %H:%M", &tm);
    widget_set_text(clock_label, text);
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
    selected = w->value;
    pw_header->value = ch_header->value = selected;
    widget_invalidate(pw_header);
    widget_invalidate(ch_header);
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
    load_background();
    if (naccounts == 0)
        return 1;
    struct widget *win = main_win =
        app_layer_window(app, 0, 0, 3, GUI_ANCHOR_TOP | GUI_ANCHOR_BOTTOM | GUI_ANCHOR_LEFT | GUI_ANCHOR_RIGHT,
                         0, 1, "greeter");
    if (!win)
        return 1;
    widget_set_padding(win, 0);
    struct utsname u;
    const char *host = uname(&u) == 0 && u.nodename[0] ? u.nodename : "minios";

    struct widget *back = container_new(&backdrop_class, win, 1, 0);
    widget_set_stretch(back, 1, 1);

    /* The top bar: the host name, the clock in the middle, the power
     * buttons on the right. */
    struct widget *bar = container_new(&bar_class, back, 0, 4);
    struct widget *host_label = label_new(bar, host);
    widget_set_stretch(host_label, 1, 0);
    clock_label = label_new(bar, "");
    struct widget *right = container_new(&clear_box_class, bar, 0, 0);
    widget_set_stretch(right, 1, 0);
    /* A transparent spacer, which leaves the centred clock visible. */
    struct widget *gap = widget_new(&spacer_class, right);
    widget_set_stretch(gap, 1, 0);
    struct widget *restart = button_new(right, _("Restart"));
    widget_connect(restart, "clicked", on_power, "reboot");
    struct widget *off = button_new(right, _("Shut down"));
    widget_connect(off, "clicked", on_power, "poweroff");

    struct widget *top = widget_new(&spacer_class, back);
    widget_set_stretch(top, 0, 1);
    card = container_new(&card_class, back, 1, 16);
    widget_set_align(card, ALIGN_CENTER, ALIGN_CENTER);
    widget_set_min(card, CARD_W, 0);
    widget_set_max(card, CARD_W, 0);
    struct widget *bottom = widget_new(&spacer_class, back);
    widget_set_stretch(bottom, 0, 2);

    /* The accounts. */
    pages[PAGE_USERS] = box_new(card, 1);
    char welcome[128];
    snprintf(welcome, sizeof welcome, _("Welcome to %s"), host);
    label_new(pages[PAGE_USERS], welcome);
    for (int i = 0; i < naccounts; i++) {
        rows[i] = row_new(pages[PAGE_USERS], i, 1);
        widget_connect(rows[i], "clicked", on_account, NULL);
    }

    /* The password of the chosen account. */
    pages[PAGE_PASSWORD] = box_new(card, 1);
    pw_header = row_new(pages[PAGE_PASSWORD], 0, 0);
    label_new(pages[PAGE_PASSWORD], _("Password"));
    password = masked_field(pages[PAGE_PASSWORD], on_login);
    message = label_new(pages[PAGE_PASSWORD], "");
    button_row(pages[PAGE_PASSWORD], _("Back"), _("Log in"), on_login, &login_button);

    /* The first password of an account that has none. */
    pages[PAGE_CHOOSE] = box_new(card, 1);
    ch_header = row_new(pages[PAGE_CHOOSE], 0, 0);
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
    tick(NULL);
    app_timer_add(app, 10000, 1, tick, NULL);
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
        if (initgroups(name, gid) < 0 || setgid(gid) < 0 || setuid(uid) < 0)
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
    if (access("/dev/fb0", R_OK | W_OK) < 0)
        fall_back("no display");
    if (start_server() < 0)
        fall_back("the display server does not answer");
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
