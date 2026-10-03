/* The Users page (docs/design/users.md): the accounts of /etc/passwd, the
 * full name and the password of the user's own account, and accounts
 * added or removed with the root password. The page changes nothing
 * itself. It runs passwd, and su with useradd, passwd and userdel, and
 * hands them the passwords through a pipe, one per line. */
#include "settings.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pwd.h>
#include <sys/wait.h>
#include <minios/account.h>

#define MAX_ROWS 64

static struct widget *list, *full_field, *current_field, *new_field, *repeat_field;
static struct widget *add_name, *add_full, *add_password, *root_field, *status;
static char row_names[MAX_ROWS][33];
static int nrows, selected = -1;

static void fill_list(void)
{
    listview_clear(list);
    nrows = 0;
    selected = -1;
    setpwent();
    struct passwd *pw;
    while ((pw = getpwent()) && nrows < MAX_ROWS) {
        if (pw->pw_uid != 0 && pw->pw_uid < ACCOUNT_FIRST_ID)
            continue;
        char line[160];
        snprintf(line, sizeof line, "%s  %s  (uid %u)", pw->pw_name, pw->pw_gecos, pw->pw_uid);
        listview_add(list, line);
        snprintf(row_names[nrows++], sizeof row_names[0], "%s", pw->pw_name);
    }
    endpwent();
}

/* Run argv with input on its standard input and its output collected in
 * out. Returns the exit status, or -1 when it did not run. */
static int run_with_input(char *const argv[], const char *input, char *out, size_t size)
{
    int in[2], res[2];
    if (pipe(in) < 0)
        return -1;
    if (pipe(res) < 0) {
        close(in[0]);
        close(in[1]);
        return -1;
    }
    pid_t pid = fork();
    if (pid == 0) {
        dup2(in[0], 0);
        dup2(res[1], 1);
        dup2(res[1], 2);
        close(in[0]);
        close(in[1]);
        close(res[0]);
        close(res[1]);
        execv(argv[0], argv);
        _exit(127);
    }
    close(in[0]);
    close(res[1]);
    write(in[1], input, strlen(input));
    close(in[1]);
    size_t used = 0;
    ssize_t n;
    while (used + 1 < size && (n = read(res[0], out + used, size - 1 - used)) > 0)
        used += (size_t)n;
    out[used] = '\0';
    close(res[0]);
    int st;
    if (pid < 0 || waitpid(pid, &st, 0) != pid)
        return -1;
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

/* Show the last line of a helper's output, or the message for success. */
static void show_result(int code, char *out, const char *success)
{
    if (code == 0) {
        widget_set_text(status, success);
        return;
    }
    size_t n = strlen(out);
    while (n && out[n - 1] == '\n')
        out[--n] = '\0';
    char *last = strrchr(out, '\n');
    widget_set_text(status, last ? last + 1 : out[0] ? out : _("The change failed"));
}

static int field_ok(const char *text)
{
    return !strpbrk(text, ":'\n\\");
}

static void clear_secret(struct widget *w)
{
    widget_set_text(w, "");
}

static int on_full_name(struct widget *w, void *args, void *arg)
{
    const char *full = widget_text(full_field);
    if (!field_ok(full)) {
        widget_set_text(status, _("The full name may not contain : ' or \\"));
        return 1;
    }
    char out[512];
    char *argv[] = { "/bin/passwd", "-n", (char *)full, NULL };
    show_result(run_with_input(argv, "", out, sizeof out), out, _("Full name changed"));
    fill_list();
    return 1;
}

static int on_password(struct widget *w, void *args, void *arg)
{
    if (strcmp(widget_text(new_field), widget_text(repeat_field)) != 0) {
        widget_set_text(status, _("The new passwords differ"));
        return 1;
    }
    char input[400], out[512];
    snprintf(input, sizeof input, "%s\n%s\n%s\n", widget_text(current_field), widget_text(new_field),
             widget_text(repeat_field));
    char *argv[] = { "/bin/passwd", NULL };
    int r = run_with_input(argv, input, out, sizeof out);
    memset(input, 0, sizeof input);
    clear_secret(current_field);
    clear_secret(new_field);
    clear_secret(repeat_field);
    show_result(r, out, _("Password changed"));
    return 1;
}

static int on_add(struct widget *w, void *args, void *arg)
{
    const char *name = widget_text(add_name), *full = widget_text(add_full);
    if (!account_name_valid(name)) {
        widget_set_text(status, _("A name starts with a lowercase letter, then letters, digits, _ or -"));
        return 1;
    }
    if (!field_ok(full)) {
        widget_set_text(status, _("The full name may not contain : ' or \\"));
        return 1;
    }
    if (!widget_text(add_password)[0]) {
        widget_set_text(status, _("The new account needs a password"));
        return 1;
    }
    char command[256], input[400], out[512];
    snprintf(command, sizeof command, "useradd -c '%s' %s && passwd %s", full, name, name);
    snprintf(input, sizeof input, "%s\n%s\n%s\n", widget_text(root_field), widget_text(add_password),
             widget_text(add_password));
    char *argv[] = { "/bin/su", "-c", command, "root", NULL };
    int r = run_with_input(argv, input, out, sizeof out);
    memset(input, 0, sizeof input);
    clear_secret(root_field);
    clear_secret(add_password);
    show_result(r, out, _("Account added"));
    if (r == 0) {
        widget_set_text(add_name, "");
        widget_set_text(add_full, "");
    }
    fill_list();
    return 1;
}

static int on_remove(struct widget *w, void *args, void *arg)
{
    if (selected < 0 || selected >= nrows) {
        widget_set_text(status, _("Select an account first"));
        return 1;
    }
    const char *name = row_names[selected];
    struct passwd *pw = getpwnam(name);
    if (!pw || pw->pw_uid == 0 || pw->pw_uid == getuid()) {
        widget_set_text(status, _("Neither root nor the own account can be removed"));
        return 1;
    }
    char command[128], input[200], out[512];
    snprintf(command, sizeof command, "userdel -r %s", name);
    snprintf(input, sizeof input, "%s\n", widget_text(root_field));
    char *argv[] = { "/bin/su", "-c", command, "root", NULL };
    int r = run_with_input(argv, input, out, sizeof out);
    memset(input, 0, sizeof input);
    clear_secret(root_field);
    show_result(r, out, _("Account removed"));
    fill_list();
    return 1;
}

static int on_select(struct widget *w, void *args, void *arg)
{
    selected = ((struct sig_select *)args)->index;
    return 1;
}

static struct widget *masked_field(struct widget *grid, int row, const char *label)
{
    row_label(grid, row, label);
    struct widget *f = textfield_new(grid, "");
    textfield_set_masked(f, 1);
    widget_set_grid(f, row, 1, 1, 1);
    return f;
}

void build_users(struct widget *page)
{
    label_new(page, _("Accounts"));
    list = listview_new(page);
    widget_set_hint(list, 0, 90);
    widget_connect(list, "selected", on_select, NULL);
    fill_list();

    struct passwd *self = getpwuid(getuid());
    separator_new(page);
    char title[96];
    snprintf(title, sizeof title, _("Your account %s"), self ? self->pw_name : "?");
    label_new(page, title);
    struct widget *grid = grid_new(page);
    grid_set_stretch(grid, -1, 1, 1);
    row_label(grid, 0, _("Full name"));
    full_field = textfield_new(grid, self ? self->pw_gecos : "");
    widget_set_grid(full_field, 0, 1, 1, 1);
    struct widget *b = button_new(grid, _("Set"));
    widget_set_grid(b, 0, 2, 1, 1);
    widget_connect(b, "clicked", on_full_name, NULL);
    current_field = masked_field(grid, 1, _("Current password"));
    new_field = masked_field(grid, 2, _("New password"));
    repeat_field = masked_field(grid, 3, _("Repeat"));
    b = button_new(grid, _("Change password"));
    widget_set_grid(b, 3, 2, 1, 1);
    widget_connect(b, "clicked", on_password, NULL);

    separator_new(page);
    label_new(page, _("Add or remove accounts"));
    grid = grid_new(page);
    grid_set_stretch(grid, -1, 1, 1);
    row_label(grid, 0, _("Name"));
    add_name = textfield_new(grid, "");
    widget_set_grid(add_name, 0, 1, 1, 1);
    row_label(grid, 1, _("Full name"));
    add_full = textfield_new(grid, "");
    widget_set_grid(add_full, 1, 1, 1, 1);
    add_password = masked_field(grid, 2, _("Password"));
    root_field = masked_field(grid, 3, _("Root password"));
    struct widget *row = box_new(page, 0);
    b = button_new(row, _("Add account"));
    widget_connect(b, "clicked", on_add, NULL);
    b = button_new(row, _("Remove selected account"));
    widget_connect(b, "clicked", on_remove, NULL);
    status = label_new(page, "");
}
