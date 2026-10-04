#pragma once
/* app_dialog and app_prompt over a given parent window, for dialogs that
 * open from another dialog, such as the file chooser. */
#include <gui/app.h>

int dialog_message(struct app *a, struct widget *parent, const char *title, const char *text,
                   const char *const *buttons, int nbuttons);
int dialog_prompt(struct app *a, struct widget *parent, const char *title, const char *label, char *buf, int size);
