#include "internal.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <wchar.h>
#include <term.h>
#include <sys/ipc.h>

struct edit *edit_open(int in, int out)
{
    struct edit *editor = calloc(1, sizeof *editor);
    if (!editor)
        return NULL;
    editor->in = in;
    editor->out = out;
    editor->history_limit = 500;
    editor->complete = edit_complete_files;
    return editor;
}

void edit_close(struct edit *editor)
{
    if (!editor)
        return;
    edit_history_limit(editor, 0);
    free(editor->history);
    free(editor);
}

void edit_set_completer(struct edit *editor, edit_completer callback, void *arg)
{
    editor->complete = callback ? callback : edit_complete_files;
    editor->complete_arg = arg;
}

static void output(struct edit *editor, const char *text)
{
    size_t length = strlen(text);
    while (length) {
        ssize_t n = write(editor->out, text, length);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        text += n;
        length -= (size_t)n;
    }
}

static size_t cells(const char *text, size_t length, size_t position, int columns)
{
    for (size_t i = 0; i < length;) {
        if (text[i] == 27 && i + 1 < length && text[i + 1] == '[') {
            i += 2;
            while (i < length && !(text[i] >= '@' && text[i] <= '~'))
                i++;
            if (i < length)
                i++;
            continue;
        }
        if (text[i] == '\n') {
            position += (size_t)columns - position % (size_t)columns;
            i++;
            continue;
        }
        mbstate_t state = {0};
        wchar_t wc;
        size_t count = mbrtowc(&wc, text + i, length - i, &state);
        int width = 1;
        if (count == (size_t)-1 || count == (size_t)-2 || !count)
            count = 1;
        else {
            width = wcwidth(wc);
            if (width < 0)
                width = 1;
        }
        if (width == 2 && position % (size_t)columns == (size_t)columns - 1)
            position++;
        position += (size_t)width;
        i += count;
    }
    return position;
}

static void move_rows(struct edit *editor, size_t rows, char direction)
{
    if (rows)
        dprintf(editor->out, "\033[%lu%c", (unsigned long)rows, direction);
}

static void redraw(struct edit *editor, const char *prompt, const char *buf)
{
    int columns = term_columns(editor->out);
    output(editor, "\r");
    move_rows(editor, editor->drawn_cursor / (size_t)(editor->columns ? editor->columns : columns), 'A');
    for (size_t row = 0; row < editor->drawn_rows; row++) {
        output(editor, "\033[K");
        if (row + 1 < editor->drawn_rows)
            output(editor, "\033[B\r");
    }
    if (editor->drawn_rows)
        move_rows(editor, editor->drawn_rows - 1, 'A');
    output(editor, "\r");
    output(editor, prompt);
    output(editor, buf);
    size_t prompt_cells = cells(prompt, strlen(prompt), 0, columns);
    size_t end = cells(buf, editor->length, prompt_cells, columns);
    size_t cursor = cells(buf, editor->cursor, prompt_cells, columns);
    /* Force delayed-wrap terminals to the same row as the console. */
    if (end && end % (size_t)columns == 0)
        output(editor, " \r");
    output(editor, "\033[K\r");
    if (end / (size_t)columns > cursor / (size_t)columns)
        move_rows(editor, end / (size_t)columns - cursor / (size_t)columns, 'A');
    if (cursor % (size_t)columns)
        dprintf(editor->out, "\033[%luC", (unsigned long)(cursor % (size_t)columns));
    editor->drawn_cursor = cursor;
    editor->drawn_rows = end / (size_t)columns + 1;
    editor->columns = columns;
}

static void erase(struct edit *editor, char *buf, size_t start, size_t end)
{
    memmove(buf + start, buf + end, editor->length - end + 1);
    editor->length -= end - start;
    editor->cursor = start;
}

static void recall(struct edit *editor, char *buf, size_t size, const char *line)
{
    strlcpy(buf, line, size);
    editor->length = editor->cursor = strlen(buf);
}

static ssize_t canonical_line(struct edit *editor, char *buf, size_t size)
{
    size_t length = 0;
    for (;;) {
        char c;
        ssize_t n = read(editor->in, &c, 1);
        if (n < 0 && errno == EINTR)
            return -EINTR;
        if (n <= 0) {
            buf[length] = 0;
            return length ? (ssize_t)length : -EIO;
        }
        if (c == '\n')
            break;
        if (length + 1 < size)
            buf[length++] = c;
    }
    buf[length] = 0;
    return (ssize_t)length;
}

ssize_t edit_readline(struct edit *editor, const char *prompt, char *buf, size_t size)
{
    if (!editor || !buf || size < 2)
        return -EINVAL;
    output(editor, prompt);
    struct termios saved;
    struct pollfd queued = {.fd = editor->in, .events = POLLIN};
    if (tcgetattr(editor->in, &saved) < 0 ||
        ((saved.c_lflag & ICANON) && poll(&queued, 1, 0) > 0 && (queued.revents & POLLIN)))
        return canonical_line(editor, buf, size);

    struct termios raw = saved;
    raw.c_lflag &= ~(ICANON | ECHO | ISIG);
    if (tcsetattr(editor->in, TCSANOW, &raw) < 0)
        return -EIO;
    output(editor, "\033[?2004h");
    editor->cursor = editor->length = 0;
    editor->columns = term_columns(editor->out);
    editor->drawn_cursor = cells(prompt, strlen(prompt), 0, editor->columns);
    editor->drawn_rows = editor->drawn_cursor / (size_t)editor->columns + 1;
    buf[0] = 0;
    char *draft = calloc(size, 1);
    if (!draft) {
        output(editor, "\033[?2004l");
        tcsetattr(editor->in, TCSANOW, &saved);
        return -ENOMEM;
    }
    size_t history = editor->history_count;
    int paste = 0, searching = 0;
    char query[256] = "";
    size_t query_length = 0, search_from = history;
    ssize_t result = 0;
    for (;;) {
        int key = edit_key_read(editor);
        if (key == KEY_PASTE_BEGIN) {
            paste = 1;
            continue;
        }
        if (key == KEY_PASTE_END) {
            paste = 0;
            continue;
        }
        if (paste && (key == '\n' || key == '\r'))
            key = ' ';
        if (key < 0 || (key == 4 && !editor->length)) {
            result = -EIO;
            break;
        }
        if (key == 3) {
            result = -EINTR;
            break;
        }
        if (key == '\n' || key == '\r') {
            result = (ssize_t)editor->length;
            break;
        }
        if (!paste && (key == 18 || searching)) {
            if (!searching) {
                searching = 1;
                query_length = 0;
                query[0] = 0;
                search_from = editor->history_count;
            } else if ((key == 127 || key == 8) && query_length) {
                query[--query_length] = 0;
                search_from = editor->history_count;
            } else if (key >= 32 && key < 256 && query_length + 1 < sizeof query) {
                query[query_length++] = (char)key;
                query[query_length] = 0;
                search_from = editor->history_count;
            } else if (key != 18) {
                searching = 0;
            }
            if (searching) {
                while (search_from) {
                    const char *line = editor->history[--search_from];
                    if (strstr(line, query)) {
                        recall(editor, buf, size, line);
                        break;
                    }
                }
                redraw(editor, prompt, buf);
                continue;
            }
        }
        size_t previous = edit_previous(buf, editor->cursor);
        size_t next = edit_next(buf, editor->length, editor->cursor);
        switch (key) {
        case 1:
        case KEY_HOME:
            editor->cursor = 0;
            break;
        case 5:
        case KEY_END:
            editor->cursor = editor->length;
            break;
        case 2:
        case KEY_LEFT:
            editor->cursor = previous;
            break;
        case 6:
        case KEY_RIGHT:
            editor->cursor = next;
            break;
        case 8:
        case 127:
            erase(editor, buf, previous, editor->cursor);
            break;
        case 4:
        case KEY_DELETE:
            erase(editor, buf, editor->cursor, next);
            break;
        case 11:
            buf[editor->cursor] = 0;
            editor->length = editor->cursor;
            break;
        case 21:
            erase(editor, buf, 0, editor->cursor);
            break;
        case 23:
        case KEY_WORD_LEFT: {
            size_t at = editor->cursor;
            while (at && buf[at - 1] == ' ')
                at = edit_previous(buf, at);
            while (at && buf[at - 1] != ' ')
                at = edit_previous(buf, at);
            if (key == 23)
                erase(editor, buf, at, editor->cursor);
            else
                editor->cursor = at;
            break;
        }
        case KEY_WORD_RIGHT:
            while (editor->cursor < editor->length && buf[editor->cursor] == ' ')
                editor->cursor = edit_next(buf, editor->length, editor->cursor);
            while (editor->cursor < editor->length && buf[editor->cursor] != ' ')
                editor->cursor = edit_next(buf, editor->length, editor->cursor);
            break;
        case 16:
        case KEY_UP:
            if (history == editor->history_count)
                strlcpy(draft, buf, size);
            if (history)
                recall(editor, buf, size, editor->history[--history]);
            break;
        case 14:
        case KEY_DOWN:
            if (history < editor->history_count) {
                history++;
                recall(editor, buf, size,
                       history == editor->history_count ? draft : editor->history[history]);
            }
            break;
        case 9:
            edit_complete(editor, buf, size);
            break;
        case 12:
            output(editor, "\r");
            move_rows(editor, editor->drawn_cursor / (size_t)editor->columns, 'A');
            editor->drawn_cursor = 0;
            break;
        default:
            if (key >= 32 && key < 256 && editor->length + 1 < size) {
                memmove(buf + editor->cursor + 1, buf + editor->cursor, editor->length - editor->cursor + 1);
                buf[editor->cursor++] = (char)key;
                editor->length++;
            }
            break;
        }
        redraw(editor, prompt, buf);
    }
    editor->cursor = editor->length;
    redraw(editor, prompt, buf);
    output(editor, result == -EINTR ? "^C\n" : "\n");
    output(editor, "\033[?2004l");
    tcsetattr(editor->in, TCSANOW, &saved);
    free(draft);
    return result;
}
