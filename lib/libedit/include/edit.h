#pragma once
#include <stddef.h>
#include <sys/types.h>

struct edit;
struct edit_completions {
    size_t start;
    char **items;
    size_t count;
};
typedef void (*edit_completer)(const char *line, size_t cursor,
                               struct edit_completions *matches, void *arg);

struct edit *edit_open(int in, int out);
void edit_close(struct edit *editor);
void edit_set_completer(struct edit *editor, edit_completer callback, void *arg);
ssize_t edit_readline(struct edit *editor, const char *prompt, char *buf, size_t size);
int edit_history_add(struct edit *editor, const char *line);
int edit_history_load(struct edit *editor, const char *path);
int edit_history_save(struct edit *editor, const char *path);
void edit_history_limit(struct edit *editor, size_t limit);
size_t edit_history_count(const struct edit *editor);
const char *edit_history_get(const struct edit *editor, size_t index);
int edit_completion_add(struct edit_completions *matches, const char *value);
void edit_complete_files(const char *line, size_t cursor,
                         struct edit_completions *matches, void *arg);
