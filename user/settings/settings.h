#pragma once
/* The settings program: /etc/desktop.conf holds the user's choices, the
 * desktop client applies them and pushes the ones X12 owns through the
 * settings protocol; other pages act on their subsystem directly. */
#include <gui/app.h>
#include <gui/widget.h>
#include <gui/model.h>
#include <stddef.h>

#define CONF_PATH "/etc/desktop.conf"
#define WALLPAPER_DIR "/usr/share/wallpapers"
#define KEYMAP_DIR "/usr/share/keymaps"
#define LAUNCHER_PATH "/etc/launcher"

extern struct app *app;

/* Configuration keys and their current values, in file order. */
const char *conf_get(const char *key);
/* Set a key and write the file at once; logs "settings: set KEY VALUE". */
int conf_set(const char *key, const char *value);
int conf_set_int(const char *key, int value);
int conf_int(const char *key, int fallback);

/* A page is a vertical box the builder fills; the sidebar shows one at a
 * time. */
/* A labelled row inside a page grid. */
struct widget *row_label(struct widget *grid, int row, const char *text);

void build_appearance(struct widget *page);
void build_display(struct widget *page);
void build_keyboard(struct widget *page);
void build_mouse(struct widget *page);
void build_sound(struct widget *page);
void build_datetime(struct widget *page);
void build_filetypes(struct widget *page);
void build_launcher(struct widget *page);
void build_system(struct widget *page);
