#pragma once
/* The application chooser as an object, for app_choose_program and the
 * tests.  The tests drive its window with messages instead of a nested
 * loop. */
#include <gui/app.h>

struct appchooser;
struct appchooser *appchooser_open(struct app *a, struct widget *parent, const char *path);
struct widget *appchooser_window(struct appchooser *c);
/* 0 while the window is open, 1 when an application was chosen (its
 * command is copied to command), -1 when the window was cancelled. */
int appchooser_state(struct appchooser *c, char *command, int size);
void appchooser_close(struct appchooser *c);
/* appchooser_add_program selects the entry of the program at path, the
 * result of the button Other program.  A program without an entry gets
 * one at the top of Other applications.  The result is 0, or -ENOEXEC
 * when path is not an executable regular file. */
int appchooser_add_program(struct appchooser *c, const char *path);
