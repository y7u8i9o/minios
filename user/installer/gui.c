/* installer-gui: the graphical front end of the installer
 * (docs/design/installer.md, P10 of docs/plan/packaging.md). It is the
 * console program of the installer environment. It starts X12 and shows
 * one page with the choices of struct plan in its own X12 session. The
 * Install button runs the back end of backend.c in a child process, so
 * that the window continues to react, and a timer shows the lines that the
 * back end appends to the log. When the child has ended, the page offers
 * the Power off button. The text installer takes its place when the medium
 * contains an answer file, when there is no display, when X12 does not
 * answer, and when the window is closed. */
#include "installer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <gui/app.h>

#define MAX_DISKS 8
#define MAX_KEYMAPS 64

static struct app *app;
static struct widget *win, *disk_box, *group_box, *keymap_box, *zone_field;
static struct widget *root_field, *user_field, *name_field, *pass_field;
static struct widget *log_list, *status, *install_button, *poweroff_button;
static char medium[16];
static char disks[MAX_DISKS][16];
static char keymaps[MAX_KEYMAPS][32];
static int ndisks, nkeymaps;
static pid_t child, server = -1;
static struct timer *poll_timer;
static long log_offset;

static const char *const groups[] = { "minimal", "standard", "desktop-system" };

static void set_status(const char *text)
{
    widget_set_text(status, text);
}

static struct widget *add_row(struct widget *grid, int row, const char *label, struct widget *field)
{
    struct widget *l = label_new(grid, label);
    widget_set_grid(l, row, 0, 1, 1);
    widget_set_align(l, ALIGN_START, ALIGN_CENTER);
    widget_set_grid(field, row, 1, 1, 1);
    widget_set_stretch(field, 1, 0);
    return field;
}

static struct widget *add_field(struct widget *grid, int row, const char *label, const char *text, int masked)
{
    struct widget *f = textfield_new(grid, text);
    if (masked)
        textfield_set_masked(f, 1);
    return add_row(grid, row, label, f);
}

/* The names of the keyboard layouts that the installer environment
 * provides, from the files of /usr/share/keymaps. */
static void load_keymaps(void)
{
    DIR *d = opendir("/usr/share/keymaps");
    struct dirent *e;
    while (d && (e = readdir(d)) && nkeymaps < MAX_KEYMAPS) {
        size_t n = strlen(e->d_name);
        if (n > 4 && strcmp(e->d_name + n - 4, ".mkm") == 0 && n - 4 < sizeof keymaps[0]) {
            memcpy(keymaps[nkeymaps], e->d_name, n - 4);
            keymaps[nkeymaps++][n - 4] = '\0';
        }
    }
    if (d)
        closedir(d);
    if (nkeymaps == 0)
        strcpy(keymaps[nkeymaps++], "us");
}

/* Append the lines that the back end wrote to the log since the last call. */
static void show_log(void)
{
    FILE *f = fopen(INST_LOG, "r");
    if (!f)
        return;
    char line[256];
    fseek(f, log_offset, SEEK_SET);
    while (fgets(line, sizeof line, f)) {
        size_t n = strlen(line);
        if (n == 0 || line[n - 1] != '\n')
            break;
        line[n - 1] = '\0';
        log_offset = ftell(f);
        listview_add(log_list, line);
        set_status(line);
    }
    fclose(f);
}

static void poll_child(void *arg)
{
    (void)arg;
    show_log();
    int st;
    if (waitpid(child, &st, WNOHANG) != child)
        return;
    show_log();
    child = 0;
    app_timer_remove(app, poll_timer);
    poll_timer = NULL;
    if (WIFEXITED(st) && WEXITSTATUS(st) == 0) {
        set_status("The installation is complete. Remove the medium and power off.");
        widget_set_enabled(poweroff_button, 1);
    } else {
        set_status("The installation failed. The lines above and " INST_LOG " give the reason.");
        widget_set_enabled(install_button, 1);
    }
}

static const char *selected_item(struct widget *box, int count, const char *const *items)
{
    int i = box->value;
    return i >= 0 && i < count ? items[i] : "";
}

static int install_clicked(struct widget *w, void *args, void *arg)
{
    (void)w; (void)args; (void)arg;
    if (child)
        return 1;
    struct plan p = { 0 };
    const char *kmaps[MAX_KEYMAPS], *dk[MAX_DISKS];
    for (int i = 0; i < nkeymaps; i++)
        kmaps[i] = keymaps[i];
    for (int i = 0; i < ndisks; i++)
        dk[i] = disks[i];
    snprintf(p.disk, sizeof p.disk, "%s", selected_item(disk_box, ndisks, dk));
    snprintf(p.group, sizeof p.group, "%s", selected_item(group_box, 3, groups));
    snprintf(p.keymap, sizeof p.keymap, "%s", selected_item(keymap_box, nkeymaps, kmaps));
    snprintf(p.timezone, sizeof p.timezone, "%s", widget_text(zone_field));
    snprintf(p.root_password, sizeof p.root_password, "%s", widget_text(root_field));
    snprintf(p.user, sizeof p.user, "%s", widget_text(user_field));
    snprintf(p.user_fullname, sizeof p.user_fullname, "%s", widget_text(name_field));
    snprintf(p.user_password, sizeof p.user_password, "%s", widget_text(pass_field));
    inst_defaults(&p);
    if (!p.disk[0]) {
        set_status("There is no disk to install on.");
        return 1;
    }
    if (!p.root_password[0] || (p.user[0] && !p.user_password[0])) {
        set_status("The passwords of root and of the account must not be empty.");
        return 1;
    }
    listview_clear(log_list);
    log_offset = 0;
    unlink(INST_LOG);
    widget_set_enabled(install_button, 0);
    set_status("Installing, please wait.");
    child = fork();
    if (child < 0) {
        child = 0;
        set_status("The installer cannot start a process.");
        widget_set_enabled(install_button, 1);
        return 1;
    }
    if (child == 0) {
        int r = inst_check(&p, medium) < 0 ? -1 : inst_install(&p);
        inst_log(r == 0 ? "done" : "the installation failed");
        _exit(r == 0 ? 0 : 1);
    }
    poll_timer = app_timer_add(app, 300, 1, poll_child, NULL);
    return 1;
}

static int poweroff_clicked(struct widget *w, void *args, void *arg)
{
    (void)w; (void)args; (void)arg;
    pid_t pid = fork();
    if (pid == 0) {
        execl("/usr/bin/initctl", "initctl", "poweroff", (char *)NULL);
        _exit(127);
    }
    return 1;
}

static int close_clicked(struct widget *w, void *args, void *arg)
{
    (void)w; (void)args; (void)arg;
    return child != 0;      /* Closing is refused while the installation runs. */
}

/* Replace the graphical installer with the text installer on the
 * console, after stopping X12, which owns the framebuffer. */
static void fall_back(const char *reason)
{
    if (reason)
        inst_log("%s, starting the text installer", reason);
    if (server > 0) {
        kill(server, SIGTERM);
        waitpid(server, NULL, 0);
    }
    execl("/usr/bin/installer", "installer", (char *)NULL);
    perror("installer-gui: /usr/bin/installer");
    exit(1);
}

/* Start X12 and wait until it accepts connections. Returns 0, or -1 when
 * it does not answer within five seconds. */
static int start_server(void)
{
    server = fork();
    if (server == 0) {
        execlp("x12", "x12", (char *)NULL);
        _exit(127);
    }
    for (int i = 0; server > 0 && i < 50; i++) {
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

int main(void)
{
    mkdir(INST_DIR, 0755);
    if (inst_find_medium(medium, sizeof medium) < 0) {
        inst_log("no installation medium with a repository for this machine was found");
        return 1;
    }
    struct stat st;
    if (stat(INST_MEDIUM "/" INST_ANSWERS, &st) == 0)
        fall_back(NULL);        /* the text installer installs without questions */
    if (access("/dev/fb0", R_OK | W_OK) < 0)
        fall_back("there is no display");
    if (start_server() < 0)
        fall_back("the display server does not answer");
    long mib[MAX_DISKS];
    ndisks = inst_list_disks(medium, disks, mib, MAX_DISKS);
    load_keymaps();
    struct plan def = { 0 };
    inst_defaults(&def);

    app = app_create();
    if (!app)
        fall_back("the window cannot connect to the display server");
    win = app_window(app, 560, 640, "Install minios");
    if (!win)
        fall_back("the window cannot be created");
    widget_connect(win, "close", close_clicked, NULL);
    struct widget *box = box_new(win, 1);
    widget_set_stretch(box, 1, 1);

    struct widget *grid = grid_new(box);
    grid_set_stretch(grid, 0, 1, 1);
    int row = 0;
    disk_box = add_row(grid, row++, "Target disk, which will be erased", combobox_new(grid));
    for (int i = 0; i < ndisks; i++) {
        char item[48];
        snprintf(item, sizeof item, "%s  (%ld MiB)", disks[i], mib[i]);
        combobox_add(disk_box, item);
    }
    if (ndisks)
        combobox_select(disk_box, 0);
    group_box = add_row(grid, row++, "Package group", combobox_new(grid));
    for (int i = 0; i < 3; i++)
        combobox_add(group_box, groups[i]);
    combobox_select(group_box, 2);
    keymap_box = add_row(grid, row++, "Keyboard layout", combobox_new(grid));
    for (int i = 0; i < nkeymaps; i++) {
        combobox_add(keymap_box, keymaps[i]);
        if (strcmp(keymaps[i], def.keymap) == 0)
            combobox_select(keymap_box, i);
    }
    zone_field = add_field(grid, row++, "Time zone", def.timezone, 0);
    root_field = add_field(grid, row++, "Password of root", "", 1);
    user_field = add_field(grid, row++, "Account name", "user", 0);
    name_field = add_field(grid, row++, "Full name", "", 0);
    pass_field = add_field(grid, row++, "Account password", "", 1);

    struct widget *buttons = box_new(box, 0);
    install_button = button_new(buttons, "Install");
    widget_connect(install_button, "clicked", install_clicked, NULL);
    poweroff_button = button_new(buttons, "Power off");
    widget_set_enabled(poweroff_button, 0);
    widget_connect(poweroff_button, "clicked", poweroff_clicked, NULL);

    log_list = listview_new(box);
    widget_set_stretch(log_list, 1, 1);
    widget_set_min(log_list, 0, 140);
    status = label_new(box, ndisks ? "Choose the options and press Install." : "There is no disk to install on.");

    app_run(app);
    app_destroy(app);
    /* The window was closed before or after an installation, or the
     * display server ended. An installation that still runs completes
     * before the console is handed over. */
    if (child) {
        int status;
        waitpid(child, &status, 0);
    }
    fall_back("the graphical installer has ended");
}
