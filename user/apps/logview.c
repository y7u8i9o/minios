/* logview: the kernel log from /dev/klog, following new lines, with a
 * filter field and a pause check box. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ipc.h>
#include <gui/app.h>

static struct app *app;
static struct widget *editor, *filter, *pause_box, *status;
static char *all;
static size_t all_len, all_cap;
static int klog_fd, lines_total;

static void rebuild(void)
{
    const char *needle = widget_text(filter);
    char *out = malloc(all_len + 1);
    size_t o = 0;
    int shown = 0;
    for (size_t i = 0; i < all_len;) {
        size_t j = i;
        while (j < all_len && all[j] != '\n')
            j++;
        size_t len = j - i;
        char line[512];
        if (len >= sizeof line)
            len = sizeof line - 1;
        memcpy(line, all + i, len);
        line[len] = '\0';
        if (!needle[0] || strstr(line, needle)) {
            memcpy(out + o, line, len);
            o += len;
            out[o++] = '\n';
            shown++;
        }
        i = j + 1;
    }
    out[o] = '\0';
    editor_set_text(editor, out);
    free(out);
    editor_goto(editor, editor_line_count(editor) - 1, 0);
    char s[64];
    snprintf(s, sizeof s, "%d of %d lines", shown, lines_total);
    widget_set_text(status, s);
}

static void on_klog(int fd, int revents, void *arg)
{
    char buf[512];
    ssize_t n = read(fd, buf, sizeof buf);
    if (n <= 0)
        return;
    if (all_len + (size_t)n + 1 > all_cap) {
        all_cap = (all_len + (size_t)n + 1) * 2;
        all = realloc(all, all_cap);
    }
    memcpy(all + all_len, buf, (size_t)n);
    all_len += (size_t)n;
    for (ssize_t i = 0; i < n; i++)
        lines_total += buf[i] == '\n';
    if (!pause_box->value)
        rebuild();
}

static int on_filter(struct widget *w, void *args, void *arg) { rebuild(); return 0; }
static int on_pause(struct widget *w, void *args, void *arg) { if (!w->value) rebuild(); return 0; }
static int on_clear(struct widget *w, void *args, void *arg) { all_len = 0; lines_total = 0; rebuild(); return 1; }

int main(void)
{
    app = app_create();
    if (!app)
        return 1;
    klog_fd = open("/dev/klog", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (klog_fd < 0) {
        perror("logview: /dev/klog");
        return 1;
    }
    struct widget *win = app_window(app, 640, 400, "logview");
    if (!win)
        return 1;
    struct widget *bar = toolbar_new(win);
    label_new(bar, "Filter:");
    filter = textfield_new(bar, "");
    widget_connect(filter, "changed", on_filter, NULL);
    pause_box = checkbox_new(bar, "Pause");
    widget_connect(pause_box, "toggled", on_pause, NULL);
    widget_connect(button_new(bar, "Clear"), "clicked", on_clear, NULL);
    editor = editor_new(win);
    editor_set_readonly(editor, 1);
    struct widget *sb = statusbar_new(win);
    status = statusbar_add(sb, 1);
    all_cap = 65536;
    all = malloc(all_cap);
    app_watch_fd(app, klog_fd, POLLIN, on_klog, NULL);
    on_klog(klog_fd, POLLIN, NULL);
    widget_focus(editor);
    app_run(app);
    app_destroy(app);
    return 0;
}
