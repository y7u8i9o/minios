/* evtest: shows the events its window receives (pointer, buttons,
 * wheel, keys with codes and characters, focus, resizes) as a log. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <gui/app.h>

static struct widget *editor, *status;
static int count;

static void add(const char *line)
{
    char *t = editor_text(editor);
    size_t n = strlen(t);
    char *joined = malloc(n + strlen(line) + 2);
    strcpy(joined, t);
    if (n)
        strcat(joined, "\n");
    strcat(joined, line);
    editor_set_text(editor, joined);
    free(joined);
    free(t);
    editor_goto(editor, editor_line_count(editor) - 1, 0);
    char s[32];
    snprintf(s, sizeof s, "%d events", ++count);
    widget_set_text(status, s);
    printf("evtest: %s\n", line);
    fflush(stdout);
}

static int on_mouse(struct widget *w, void *args, void *arg)
{
    struct sig_click *c = args;
    char line[96];
    snprintf(line, sizeof line, "%s at %d,%d buttons %d", (const char *)arg, c->x, c->y, c->button);
    add(line);
    return 1;
}

static int on_key(struct widget *w, void *args, void *arg)
{
    struct sig_key *k = args;
    char line[96];
    snprintf(line, sizeof line, "%s code 0x%02x mods %d char %d%s%c%s", (const char *)arg, k->code, k->mods, k->ch,
             k->ch >= 32 && k->ch < 127 ? " '" : "", k->ch >= 32 && k->ch < 127 ? (char)k->ch : ' ',
             k->ch >= 32 && k->ch < 127 ? "'" : "");
    add(line);
    return 1;
}

static int on_resize(struct widget *w, void *args, void *arg)
{
    struct sig_resize *r = args;
    char line[64];
    snprintf(line, sizeof line, "resize %dx%d", r->w, r->h);
    add(line);
    return 0;
}

static int on_focus(struct widget *w, void *args, void *arg)
{
    add(((struct sig_change *)args)->value ? "focus in" : "focus out");
    return 0;
}

int main(void)
{
    struct app *app = app_create();
    if (!app)
        return 1;
    struct widget *win = app_window(app, 480, 360, "evtest");
    if (!win)
        return 1;
    struct widget *canvas = canvas_new(win);
    widget_set_hint(canvas, 0, 80);
    widget_set_stretch(canvas, 1, 0);
    widget_connect(canvas, "press", on_mouse, "press");
    widget_connect(canvas, "release", on_mouse, "release");
    widget_connect(canvas, "wheel", on_mouse, "wheel");
    widget_connect(canvas, "key", on_key, "key");
    widget_connect(canvas, "keyup", on_key, "keyup");
    widget_connect(win, "resize", on_resize, NULL);
    widget_connect(win, "focus", on_focus, NULL);
    editor = editor_new(win);
    editor_set_readonly(editor, 1);
    struct widget *sb = statusbar_new(win);
    status = statusbar_add(sb, 1);
    widget_set_text(status, "click, scroll or type in the area at the top");
    widget_focus(canvas);
    app_run(app);
    app_destroy(app);
    return 0;
}
