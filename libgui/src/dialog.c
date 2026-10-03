/* Modal dialogs: a second window driven by nested app_step calls until
 * a button is chosen or the window is closed. */
#include <gui/app.h>
#include <string.h>
#include <libintl.h>
#include "dialog.h"

struct dialog {
    struct app *app;
    struct widget *win;
    int result, done;
    struct widget *field;
};

static int on_button(struct widget *w, void *args, void *arg)
{
    struct dialog *d = arg;
    int i = 0;
    for (struct widget *b = w->parent->first; b && b != w; b = b->next)
        i++;
    d->result = i;
    d->done = 1;
    return 1;
}

static int on_close(struct widget *w, void *args, void *arg)
{
    struct dialog *d = arg;
    d->result = -1;
    d->done = 1;
    return 1;
}

static int on_activate(struct widget *w, void *args, void *arg)
{
    struct dialog *d = arg;
    d->result = 0;
    d->done = 1;
    return 1;
}

static void run(struct dialog *d)
{
    while (!d->done && app_step(d->app, -1))
        ;
    window_close(d->win);
    app_step(d->app, 0);
}

int dialog_message(struct app *a, struct widget *parent, const char *title, const char *text,
                   const char *const *buttons, int nbuttons)
{
    const struct theme *t = app_theme(a);
    int width = gfx_text_width_font(t->font, text, -1) + 40;
    if (width < 240)
        width = 240;
    struct dialog d = { a, NULL, -1, 0, NULL };
    d.win = app_modal_window(a, parent, width, 3 * theme_px(t, TM_CONTROL_H) + 30, title);
    if (!d.win)
        return -1;
    label_new(d.win, text);
    struct widget *row = box_new(d.win, 0);
    for (int i = 0; i < nbuttons; i++) {
        struct widget *b = button_new(row, buttons[i]);
        widget_set_stretch(b, 1, 0);
        widget_connect(b, "clicked", on_button, &d);
        if (i == 0)
            widget_focus(b);
    }
    widget_connect(d.win, "close", on_close, &d);
    run(&d);
    return d.result;
}

int dialog_prompt(struct app *a, struct widget *parent, const char *title, const char *label, char *buf, int size)
{
    const struct theme *t = app_theme(a);
    struct dialog d = { a, NULL, -1, 0, NULL };
    d.win = app_modal_window(a, parent, 360, 4 * theme_px(t, TM_CONTROL_H) + 30, title);
    if (!d.win)
        return 0;
    label_new(d.win, label);
    d.field = textfield_new(d.win, buf);
    widget_connect(d.field, "activate", on_activate, &d);
    struct widget *row = box_new(d.win, 0);
    struct widget *ok = button_new(row, dgettext("libgui", "OK")), *cancel = button_new(row, dgettext("libgui", "Cancel"));
    widget_set_stretch(ok, 1, 0);
    widget_set_stretch(cancel, 1, 0);
    widget_connect(ok, "clicked", on_button, &d);
    widget_connect(cancel, "clicked", on_button, &d);
    widget_connect(d.win, "close", on_close, &d);
    widget_focus(d.field);
    int ok_pressed = 0;
    while (!d.done && app_step(a, -1))
        ;
    if (d.result == 0) {
        strlcpy(buf, widget_text(d.field), (size_t)size);
        ok_pressed = 1;
    }
    window_close(d.win);
    app_step(a, 0);
    return ok_pressed;
}

int app_dialog(struct app *a, const char *title, const char *text, const char *const *buttons, int nbuttons)
{
    return dialog_message(a, app_first_window(a), title, text, buttons, nbuttons);
}

int app_prompt(struct app *a, const char *title, const char *label, char *buf, int size)
{
    return dialog_prompt(a, app_first_window(a), title, label, buf, size);
}
