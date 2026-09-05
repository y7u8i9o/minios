#include "internal.h"
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/ipc.h>

static int byte(struct edit *editor, int timeout)
{
    if (timeout >= 0) {
        struct pollfd fd = {.fd = editor->in, .events = POLLIN};
        if (poll(&fd, 1, timeout) <= 0)
            return -1;
    }
    unsigned char c;
    ssize_t count;
    do {
        count = read(editor->in, &c, 1);
    } while (count < 0 && errno == EINTR);
    return count == 1 ? c : -1;
}

int edit_key_read(struct edit *editor)
{
    int c = byte(editor, -1);
    if (c != 27)
        return c;
    c = byte(editor, 100);
    if (c == 'b' || c == 'B')
        return KEY_WORD_LEFT;
    if (c == 'f' || c == 'F')
        return KEY_WORD_RIGHT;
    if (c != '[' && c != 'O')
        return 27;
    char sequence[24];
    size_t n = 0;
    while (n + 1 < sizeof sequence) {
        c = byte(editor, 100);
        if (c < 0)
            return 27;
        sequence[n++] = (char)c;
        if (c >= 0x40 && c <= 0x7e)
            break;
    }
    sequence[n] = 0;
    if (!strcmp(sequence, "A"))
        return KEY_UP;
    if (!strcmp(sequence, "B"))
        return KEY_DOWN;
    if (!strcmp(sequence, "C"))
        return KEY_RIGHT;
    if (!strcmp(sequence, "D"))
        return KEY_LEFT;
    if (!strcmp(sequence, "H") || !strcmp(sequence, "1~") || !strcmp(sequence, "7~"))
        return KEY_HOME;
    if (!strcmp(sequence, "F") || !strcmp(sequence, "4~") || !strcmp(sequence, "8~"))
        return KEY_END;
    if (!strcmp(sequence, "3~"))
        return KEY_DELETE;
    if (!strcmp(sequence, "200~"))
        return KEY_PASTE_BEGIN;
    if (!strcmp(sequence, "201~"))
        return KEY_PASTE_END;
    return 27;
}

size_t edit_previous(const char *text, size_t position)
{
    if (!position)
        return 0;
    position--;
    while (position && ((unsigned char)text[position] & 0xc0) == 0x80)
        position--;
    return position;
}

size_t edit_next(const char *text, size_t length, size_t position)
{
    if (position >= length)
        return length;
    position++;
    while (position < length && ((unsigned char)text[position] & 0xc0) == 0x80)
        position++;
    return position;
}
