#pragma once
#include <edit.h>
#include <termios.h>

struct edit {
    int in, out;
    edit_completer complete;
    void *complete_arg;
    char **history;
    size_t history_count, history_limit;
    size_t cursor, length, drawn_cursor, drawn_rows;
    int columns;
};

enum edit_key {
    KEY_LEFT = 256, KEY_RIGHT, KEY_UP, KEY_DOWN, KEY_HOME, KEY_END,
    KEY_DELETE, KEY_WORD_LEFT, KEY_WORD_RIGHT, KEY_PASTE_BEGIN, KEY_PASTE_END
};
int edit_key_read(struct edit *editor);
size_t edit_previous(const char *text, size_t position);
size_t edit_next(const char *text, size_t length, size_t position);
void edit_complete(struct edit *editor, char *buf, size_t size);
