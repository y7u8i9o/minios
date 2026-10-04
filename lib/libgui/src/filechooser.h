#pragma once
/* The file chooser as an object, for app_choose_file and the tests,
 * which drive its window with messages instead of a nested loop. */
#include <gui/app.h>

struct chooser;
struct chooser *chooser_open(struct app *a, struct widget *parent, enum file_chooser_mode mode, const char *title,
                             const struct file_filter *filters, int nfilters, const char *path);
struct widget *chooser_window(struct chooser *c);
/* 0 while the window is open, 1 when a file was chosen (its path is
 * copied to path), -1 when it was cancelled. */
int chooser_state(struct chooser *c, char *path, int size);
void chooser_close(struct chooser *c);
