#pragma once
/* imed: the input method daemon (I1, docs/design/ime.md).  It holds the
 * input method of the seat, gives the keys of the active text input
 * context to the selected engine and shows the candidates of the engine in
 * its candidate window.  Single threaded. */
#include <stddef.h>
#include <stdint.h>

#define IMED_MAX_CANDIDATES 128
#define IMED_CANDIDATE_BYTES 96

/* An engine.  key receives a press with the character of the layout (0
 * for a key without one) and the modifiers of gui/keymap.h, and returns 1
 * when the engine used the key.  flush commits the composition as it is
 * shown, reset drops it.  init may be NULL.  Every function may change the composition
 * through the functions below. */
struct imed_engine {
    const char *name, *label, *title;
    void (*init)(void);                 /* at the start of the daemon: loads the dictionaries */
    void (*select)(void);
    int (*key)(uint32_t key, int ch, int mods);
    void (*flush)(void);
    void (*reset)(void);
    void (*candidate_clicked)(int index);
};

/* The lookup table of the candidate window, as in IBus: the candidates,
 * an optional comment for each, the cursor, the page size, the labels and
 * an auxiliary line above the candidates. */
struct imed_table {
    char text[IMED_MAX_CANDIDATES][IMED_CANDIDATE_BYTES];
    char comment[IMED_MAX_CANDIDATES][32];
    int n, cursor, page_size;
    int vertical;
    char aux[256];
};

extern struct imed_table imed_table;

/* The changes of the composition, sent together by imed_flush after each
 * event. */
void imed_commit(const char *text);
void imed_preedit(const char *text, int cursor);     /* cursor in bytes, -1 for the end */
void imed_table_changed(void);                      /* shows, hides or redraws the window */
void imed_set_label(const char *label);             /* the label of the current engine */
const char *imed_config(const char *key);           /* a value of desktop.conf, or "" */
int imed_page_first(void);                          /* the first candidate of the page of the cursor */

/* The connection, for window.c. */
struct wire_proxy;
struct wire_display;
extern struct wire_display *display;
extern struct wire_proxy *compositor, *shm, *im;

/* The candidate window (window.c). */
int window_owns(const struct wire_proxy *surface);
void window_init(void);
void window_update(int shown);
int window_click(int x, int y);                     /* the candidate at a point, or -1 */
void window_scroll(int pages);
void window_set_scale(int scale);

/* The engines. */
extern const struct imed_engine test_engine, pinyin_engine, japanese_engine;
