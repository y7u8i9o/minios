/* The interactive prompt of /bin/lua edits lines with libedit. It moves
 * the cursor, recalls the history with the arrow keys and Ctrl+R,
 * completes global names and table fields with Tab, and maintains a
 * history file, $HOME/.lua_history, which is loaded at the first
 * prompt and written at exit. lreadline.h maps the readline hooks of
 * lua.c to these functions. The host test program has no prompt and
 * compiles none of this. */
#ifndef MINIOS_HOST
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <edit.h>
#include "lua.h"
#include "lreadline.h"

#define LINE_BYTES 4096
#define HISTORY_LIMIT 500

static struct edit *editor;
static char *history_path;
static char line[LINE_BYTES];

static void history_write(void)
{
    if (editor && history_path)
        edit_history_save(editor, history_path);
}

static int name_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

/* The completer offers the globals, or the fields of the table that a
 * dotted chain of names leads to, that begin with the name before the
 * cursor ("string.fo" offers "string.format"). It reads the tables
 * without metamethods. */
static void complete_names(const char *text, size_t cursor, struct edit_completions *matches, void *arg)
{
    lua_State *L = arg;
    size_t start = cursor;
    while (start > 0 && (name_char(text[start - 1]) || text[start - 1] == '.'))
        start--;
    if (start > 0 && text[start - 1] == ':')
        return;
    matches->start = start;
    int top = lua_gettop(L);
    lua_pushglobaltable(L);
    size_t at = start;
    for (;;) {
        size_t n = 0;
        while (at + n < cursor && text[at + n] != '.')
            n++;
        if (at + n == cursor) {
            /* The last part is the prefix to complete in the table on top. */
            if (lua_istable(L, -1)) {
                lua_pushnil(L);
                while (lua_next(L, -2)) {
                    lua_pop(L, 1);
                    if (lua_type(L, -1) != LUA_TSTRING)
                        continue;
                    size_t klen;
                    const char *key = lua_tolstring(L, -1, &klen);
                    if (klen < n || strncmp(key, text + at, n) != 0 || strlen(key) != klen)
                        continue;
                    char item[256];
                    if ((size_t)snprintf(item, sizeof item, "%.*s%s", (int)(at - start), text + start, key) < sizeof item)
                        edit_completion_add(matches, item);
                }
            }
            break;
        }
        if (!lua_istable(L, -1))
            break;
        lua_pushlstring(L, text + at, n);
        lua_rawget(L, -2);
        lua_remove(L, -2);
        at += n + 1;
    }
    lua_settop(L, top);
}

void minios_readline_init(lua_State *L)
{
    if (editor)
        return;
    editor = edit_open(0, 1);
    if (!editor)
        return;
    edit_history_limit(editor, HISTORY_LIMIT);
    edit_set_completer(editor, complete_names, L);
    const char *home = getenv("HOME");
    if (home && *home) {
        size_t size = strlen(home) + sizeof "/.lua_history";
        history_path = malloc(size);
        if (history_path) {
            snprintf(history_path, size, "%s/.lua_history", home);
            edit_history_load(editor, history_path);
        }
    }
    atexit(history_write);
}

/* This returns a line without its newline, or NULL at the end of the
 * input. Ctrl+C discards the line being edited and returns an empty
 * one. */
char *minios_readline(const char *prompt)
{
    if (!editor) {
        fputs(prompt, stdout);
        fflush(stdout);
        return fgets(line, sizeof line, stdin);
    }
    fflush(stdout);
    ssize_t n = edit_readline(editor, prompt, line, sizeof line);
    if (n == -EINTR) {
        line[0] = '\0';
        return line;
    }
    return n < 0 ? NULL : line;
}

/* lua.c saves a whole statement, which may span several lines; each
 * line enters the history on its own, so that recalling one edits one. */
void minios_readline_save(const char *text)
{
    if (!editor)
        return;
    while (*text) {
        size_t n = strcspn(text, "\n");
        if (n > 0 && n < LINE_BYTES) {
            memcpy(line, text, n);
            line[n] = '\0';
            edit_history_add(editor, line);
        }
        text += n;
        if (*text)
            text++;
    }
}
#endif
