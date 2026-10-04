#pragma once
/* The folder view shared by the file manager and the file chooser, in
 * the manner of GNOME's Files and its file chooser.  It consists of a
 * places sidebar (Recent, the home folder and the folders below it that exist, the
 * system folders, mounted volumes and the root), a path bar of folder
 * buttons that retains the folders below the current one, a location
 * entry with inline completion (Ctrl+L, or typing / or ~), a search
 * below the folder started by typing, and a table of the entries with
 * the name, size, type and modification time.  The listing is reread
 * every two seconds, so files written by other programs appear.  The
 * folder view is freed with its widgets.
 *
 * Rows can be dragged to other programs as text/uri-list and text/plain,
 * and files dropped on a folder row, the empty part of the table or a
 * place of the sidebar are copied or moved there (gui/fileops.h).
 *
 * Keys handled on the window: typing over the table or the sidebar,
 * Ctrl+L, Ctrl+F, Ctrl+H (hidden files), Alt+Up and Backspace (the
 * parent folder), Alt+Down (back down the path bar), Alt+Home, and
 * Escape while the location entry or the search is shown. */
#include <gui/widget.h>
#include <stdint.h>

struct folderview;

enum folderview_kind { FOLDERVIEW_FOLDER, FOLDERVIEW_RECENT, FOLDERVIEW_SEARCH };

struct folderview_entry {
    char *path;                 /* full path */
    const char *name;           /* its last component */
    long size;
    int64_t mtime;
    int dir, exec;
    const char *desc;           /* the type as shown in the table */
    const struct image *icon;
};

/* Callbacks of the program, each of which may be NULL. */
struct folderview_ops {
    /* A file was activated by Enter, a double click or the location entry. */
    void (*open)(struct folderview *fv, const char *path, void *arg);
    /* The folder, the kind of view or the listing changed. */
    void (*changed)(struct folderview *fv, void *arg);
    /* The selected entry changed. */
    void (*selected)(struct folderview *fv, void *arg);
    /* Whether a file (never a folder) is listed, NULL to list every file. */
    int (*filter)(struct folderview *fv, const char *name, void *arg);
    /* A key no widget consumed, offered before the folder view's own
     * keys.  The result is 1 when the program handled it. */
    int (*key)(struct folderview *fv, const struct sig_key *k, void *arg);
};

/* The path bar, the location entry, the search field and the search
 * button are added to bar, a horizontal box or tool bar, the sidebar and
 * the table to split, a horizontal split pane.  title names the error
 * dialogs.  The folder view starts in the home folder. */
struct folderview *folderview_new(struct widget *bar, struct widget *split, const char *title,
                                  const struct folderview_ops *ops, void *arg);
struct widget *folderview_table(struct folderview *fv);
struct widget *folderview_sidebar(struct folderview *fv);

/* fn is called after files dropped into the folder dir were copied or
 * moved (action GUI_DND_*). */
void folderview_on_dropped(struct folderview *fv,
                           void (*fn)(struct folderview *fv, const char *dir, int count, int action, void *arg));

/* Show a folder and report an error in a dialog.  Returns 0 or -errno. */
int folderview_navigate(struct folderview *fv, const char *path);
void folderview_show_recent(struct folderview *fv);
/* Show the search field with text, below the current folder. */
void folderview_search(struct folderview *fv, const char *text);
/* Show the location entry with text, or the current folder for NULL. */
void folderview_location(struct folderview *fv, const char *text);
void folderview_up(struct folderview *fv);
void folderview_home(struct folderview *fv);
/* Leave Recent or a search for the folder of the selected entry, with
 * the entry selected. */
void folderview_visit(struct folderview *fv);
/* Reread the view, retaining the selection by name. */
void folderview_reload(struct folderview *fv);
void folderview_select_name(struct folderview *fv, const char *name);

enum folderview_kind folderview_kind(const struct folderview *fv);
const char *folderview_cwd(const struct folderview *fv);
/* The folder of an absolute or relative name typed by the user, with ~
 * for the home folder, into out. */
void folderview_resolve(const struct folderview *fv, const char *text, char *out, size_t size);
const char *folderview_home_path(const struct folderview *fv);
const struct folderview_entry *folderview_selected(const struct folderview *fv);
int folderview_count(const struct folderview *fv);
/* Hidden entries of the folder that are not listed. */
int folderview_hidden_count(const struct folderview *fv);
void folderview_set_hidden(struct folderview *fv, int show);
int folderview_hidden(const struct folderview *fv);
/* Sort by column 0 for the name, 1 for the size (the location in Recent
 * and searches), 2 for the type and 3 for the modification time. */
void folderview_sort(struct folderview *fv, int column, int descending);

/* Put a path at the head of the recent list ($HOME/.local/share/recent-files). */
void folderview_recent_add(const char *path);
/* "12 bytes", "4.2 kB", "3.1 MB" into buf. */
const char *folderview_format_size(long size, char *buf, size_t bufsize);
/* The type shown for a MIME type ("Folder", "PNG image", "Program"). */
const char *folderview_describe(const char *type, int exec);
